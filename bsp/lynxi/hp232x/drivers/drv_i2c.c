/*
 * drv_i2c.c — DesignWare I2C0 slave + MCU register / mailbox protocol.
 *
 * Two Host paths share reg=0x00:
 *  1) Legacy READ_LOG / error (hp640): short write cmd 0x01, then read raw data.
 *  2) MCU Mode B mailbox (I2C_PROTOCOL.md): Mem_Write [hdr+payload] → KA200
 *     accesses on-chip IP → Mem_Read response frame (cmd/len/crc/rsvd/data).
 */

#include <rtthread.h>
#include <string.h>
#include "drv_i2c.h"
#include "biz_i2c_proxy.h"
#include "biz_log.h"
#include "biz_ipc.h"
#include "lynxi.h"
#include "tick.h"
#include <rthw.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define IC_CLK                      100
#define NANO_TO_MICRO               1000
#define MIN_FS_SCL_HIGHTIME         1900
#define MIN_FS_SCL_LOWTIME          3020

#define IC_CON_BUS_CLEAR            0x0800U
#define IC_CON_RE                   0x0020U
#define IC_CON_SPD_FS               0x0004U
#define IC_CON_SPD_MSK              0x0006U

#define IC_SCL_STUCKLOW             0x4000U
#define IC_RESTART_DET              0x1000U
#define IC_GEN_CALL                 0x0800U
#define IC_START_DET                0x0400U
#define IC_STOP_DET                 0x0200U
#define IC_ACTIVITY                 0x0100U
#define IC_RX_DONE                  0x0080U
#define IC_TX_ABRT                  0x0040U
#define IC_RD_REQ                   0x0020U
#define IC_TX_EMPTY                 0x0010U
#define IC_RX_FULL                  0x0004U

#define IC_ENABLE_SDA_STUCK         0x08U
#define IC_STATUS_SDA_STUCK_NOT_OK  0x800U
#define IC_TX_ABRT_SDA_STUCK        0x2000U
#define IC_ENABLE_0B                0x0001U
#define IC_RX_TL                    0x00U
#define IC_TX_TL                    0x00U

#define MCU_I2C_REG_CMD_DATA        BIZ_I2C_MAILBOX_REG
#define MCU_I2C_REG_DATA            0x02U
#define MCU_I2C_CMD_READ_LOG        BIZ_I2C_CMD_READ_LOG
#define MCU_I2C_MAX_DATA_LEN        BIZ_I2C_BUF_MAX
#define MCU_I2C_TX_FIFO_BURST       8U

#define MCU_I2C_GPIO_COUNT          3U
#define MCU_I2C_GPIO_IOC_CFG_VAL    0x608U
/* Synopsys DW APB GPIO — same offsets as hp640 dwapb_gpio.c */
#define GPIO_SWPORT_DDR(bank)       (0x04U + (uint32_t)(bank) * 0x0CU)
#define GPIO_EXT_PORT_OFFSET(bank)  (0x50U + (bank) * 4U)

/* Same as hp640 device_tree_output_evb.dts i2c_mux cs-gpios / cs-iocfg-regs.
 * NOTE: iocfg[0]=0x1200014C is also FLASH_IOC_SPI2_REG (leave-XIP writes 0x660a).
 * Resolve must sample with GPIO+DDR-in, then restore iocfg for SSI. */
static const struct {
    uint32_t iocfg_reg;
    uint8_t  bank;
    uint8_t  pin;
} s_mcu_i2c_gpio_map[MCU_I2C_GPIO_COUNT] = {
    { 0x1200014CU, 2, 10 }, /* portc pin 0xa — mux bit0; shared with SSI SPI2 */
    { 0x1200015CU, 2,  2 }, /* portc pin 0x2 — mux bit1 */
    { 0x1200016CU, 2,  6 }, /* portc pin 0x6 — mux bit2 */
};

#define I2C_MCU_TIMEOUT_MS          3000U
#define I2C_STD_TIMEOUT_MS          1000U

typedef enum {
    MCU_I2C_STATE_IDLE = 0,
    MCU_I2C_STATE_CMD_RECEIVED,
    MCU_I2C_STATE_DATA_READY,
    MCU_I2C_STATE_SENDING_LEN,
    MCU_I2C_STATE_SENDING_DATA,
} mcu_i2c_state_t;

typedef struct {
    mcu_i2c_state_t protocol_state;
    uint8_t prepared_data[MCU_I2C_MAX_DATA_LEN];
    uint16_t prepared_data_len;
    uint16_t data_send_idx;
    uint16_t write_byte_idx;
    uint8_t rx_frame[MCU_I2C_MAX_DATA_LEN];
    uint16_t rx_frame_len;
    uint8_t current_reg;
    uint8_t received_cmd;
    uint8_t saw_read;   /* RD_REQ happened in this transfer */
    uint8_t saw_write;  /* RX_FULL after reg in this transfer */
} drv_i2c_slave_data_t;

typedef struct {
    volatile uint32_t ic_con;
    volatile uint32_t ic_tar;
    volatile uint32_t ic_sar;
    volatile uint32_t ic_hs_maddr;
    volatile uint32_t ic_cmd_data;
    volatile uint32_t ic_ss_scl_hcnt;
    volatile uint32_t ic_ss_scl_lcnt;
    volatile uint32_t ic_fs_scl_hcnt;
    volatile uint32_t ic_fs_scl_lcnt;
    volatile uint32_t ic_hs_scl_hcnt;
    volatile uint32_t ic_hs_scl_lcnt;
    volatile uint32_t ic_intr_stat;
    volatile uint32_t ic_intr_mask;
    volatile uint32_t ic_raw_intr_stat;
    volatile uint32_t ic_rx_tl;
    volatile uint32_t ic_tx_tl;
    volatile uint32_t ic_clr_intr;
    volatile uint32_t ic_clr_rx_under;
    volatile uint32_t ic_clr_rx_over;
    volatile uint32_t ic_clr_tx_over;
    volatile uint32_t ic_clr_rd_req;
    volatile uint32_t ic_clr_tx_abrt;
    volatile uint32_t ic_clr_rx_done;
    volatile uint32_t ic_clr_activity;
    volatile uint32_t ic_clr_stop_det;
    volatile uint32_t ic_clr_start_det;
    volatile uint32_t ic_clr_gen_call;
    volatile uint32_t ic_enable;
    volatile uint32_t ic_status;
    volatile uint32_t ic_txflr;
    volatile uint32_t ic_rxflr;
    volatile uint32_t ic_sda_hold;
    volatile uint32_t ic_tx_abrt_source;
    uint8_t res1[0x18];
    volatile uint32_t ic_enable_status;
} dw_i2c_regs_t;

typedef struct {
    dw_i2c_regs_t *regs;
    uint8_t chip_addr;
    int initialized;
    int slave_enabled;
    int irq_installed;
    drv_i2c_slave_data_t slave;
} drv_i2c_ctx_t;

static drv_i2c_ctx_t s_i2c;
static struct rt_semaphore s_i2c_irq_sem;

#define I2C0_IRQ_NUM                63
#define GICD_IGROUPR_OFFSET         0x80
#define GICD_IGRPMODR_OFFSET        0xD00

static inline void mmio_write32(uint32_t addr, uint32_t val)
{
    REG32(addr) = val;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return REG32(addr);
}

static int drv_i2c_mcu_validate_addr(uint8_t addr)
{
    return (addr >= DRV_I2C_ADDR_MIN && addr <= DRV_I2C_ADDR_MAX && addr < 0x78U) ? 0 : -1;
}

static void drv_i2c_gpio_dir_input(uint8_t bank, uint8_t pin)
{
    uint32_t addr = (uint32_t)GPIO_BASE + GPIO_SWPORT_DDR(bank);
    uint32_t ddr = mmio_read32(addr);

    mmio_write32(addr, ddr & ~(1U << pin));
}

static uint32_t drv_i2c_gpio_read_pin(uint8_t bank, uint8_t pin)
{
    return (mmio_read32((uint32_t)GPIO_BASE + GPIO_EXT_PORT_OFFSET(bank)) >> pin) & 1U;
}

static int drv_i2c_gpio_sample_mux(int bits[MCU_I2C_GPIO_COUNT])
{
    int mux = 0;
    int i;

    for (i = 0; i < (int)MCU_I2C_GPIO_COUNT; i++)
    {
        bits[i] = (int)drv_i2c_gpio_read_pin(s_mcu_i2c_gpio_map[i].bank,
                                             s_mcu_i2c_gpio_map[i].pin);
        mux |= bits[i] << i;
    }
    return mux;
}

int drv_i2c_mcu_resolve_addr(void)
{
    uint32_t saved_iocfg[MCU_I2C_GPIO_COUNT];
    int bits1[MCU_I2C_GPIO_COUNT];
    int bits2[MCU_I2C_GPIO_COUNT];
    int mux1, mux2, mux;
    int i;

    /*
     * Align hp640 (dwapb + lite_i2c_mux):
     *  - iocfg→0x608, DDR input, sample EXT_PORT
     *  - hp640 probe often gets bit0 wrong first (mux=6), then
     *    lynchip_get_mcu_i2c_addr() re-parses to mux=7 before mcu_i2c_init.
     *    RTT: double-sample after settle; use 2nd (warn if mismatch).
     *  - restore iocfg (bit0/0x14C shared with Flash SSI SPI2).
     */
    for (i = 0; i < (int)MCU_I2C_GPIO_COUNT; i++)
    {
        saved_iocfg[i] = mmio_read32(s_mcu_i2c_gpio_map[i].iocfg_reg);
        mmio_write32(s_mcu_i2c_gpio_map[i].iocfg_reg, MCU_I2C_GPIO_IOC_CFG_VAL);
        drv_i2c_gpio_dir_input(s_mcu_i2c_gpio_map[i].bank,
                               s_mcu_i2c_gpio_map[i].pin);
    }

    rt_hw_us_delay(100);
    mux1 = drv_i2c_gpio_sample_mux(bits1);
    rt_hw_us_delay(100);
    mux2 = drv_i2c_gpio_sample_mux(bits2);

    for (i = 0; i < (int)MCU_I2C_GPIO_COUNT; i++)
        mmio_write32(s_mcu_i2c_gpio_map[i].iocfg_reg, saved_iocfg[i]);

    /* Prefer 2nd sample — matches hp640 re-parse used for real SAR setup */
    mux = mux2;
    if (mux1 != mux2)
    {
        BIZ_WARN("MCU I2C GPIO mux unstable: 1st=%d bits=%d%d%d → 2nd=%d bits=%d%d%d "
                 "(use 2nd; bit0/iocfg0 often flaky after leave-XIP)\n",
                 mux1, bits1[2], bits1[1], bits1[0],
                 mux2, bits2[2], bits2[1], bits2[0]);
    }

    uint8_t addr = (uint8_t)(DRV_I2C_SAR_CHIP_0 + mux);
    if (drv_i2c_mcu_validate_addr(addr) != 0)
    {
        BIZ_WARN("MCU I2C GPIO mux invalid (mux=%d bits=%d%d%d), use 0x%02x\n",
                 mux, bits2[2], bits2[1], bits2[0], DRV_I2C_MCU_DEFAULT_ADDR);
        return (int)DRV_I2C_MCU_DEFAULT_ADDR;
    }

    BIZ_INFO("MCU I2C addr parsed: 0x%02x (mux=%d bits=%d%d%d iocfg0 was 0x%x)\n",
             addr, mux, bits2[2], bits2[1], bits2[0], saved_iocfg[0]);
    return (int)addr;
}

static inline uint32_t i2c_readl(volatile uint32_t *reg)
{
    return *reg;
}

static inline void i2c_writel(uint32_t val, volatile uint32_t *reg)
{
    *reg = val;
}

static int drv_i2c_hw_enable(int enable)
{
    int timeout = 100;
    uint32_t ena = enable ? IC_ENABLE_0B : 0U;

    do {
        i2c_writel(ena, &s_i2c.regs->ic_enable);
        if ((i2c_readl(&s_i2c.regs->ic_enable_status) & IC_ENABLE_0B) == ena)
            return 0;
        rt_hw_us_delay(25);
    } while (--timeout > 0);

    return -1;
}

static void drv_i2c_hw_set_speed(int speed)
{
    uint32_t cntl;
    uint32_t hcnt;
    uint32_t lcnt;

    drv_i2c_hw_enable(0);

    cntl = i2c_readl(&s_i2c.regs->ic_con) & ~IC_CON_SPD_MSK;
    cntl |= IC_CON_SPD_FS;
    hcnt = (IC_CLK * MIN_FS_SCL_HIGHTIME) / NANO_TO_MICRO;
    lcnt = (IC_CLK * MIN_FS_SCL_LOWTIME) / NANO_TO_MICRO;
    i2c_writel(hcnt, &s_i2c.regs->ic_fs_scl_hcnt);
    i2c_writel(lcnt, &s_i2c.regs->ic_fs_scl_lcnt);
    i2c_writel(cntl, &s_i2c.regs->ic_con);

    (void)speed;
}

static void drv_i2c_hw_set_intr_mask(int enable)
{
    uint32_t cntl = i2c_readl(&s_i2c.regs->ic_intr_mask);

    if (enable)
    {
        cntl |= IC_RX_FULL | IC_RD_REQ | IC_RX_DONE | IC_GEN_CALL |
                IC_STOP_DET | IC_START_DET | IC_RESTART_DET;
        cntl &= ~IC_TX_EMPTY;
    }
    else
    {
        cntl = 0;
    }

    i2c_writel(cntl, &s_i2c.regs->ic_intr_mask);
}

static int drv_i2c_hw_init_slave(uint8_t addr)
{
    s_i2c.regs = (void *)(uintptr_t)DRV_I2C0_BASE;

    if (drv_i2c_hw_enable(0) != 0)
        return -1;

    i2c_writel(IC_CON_RE | IC_CON_SPD_FS | IC_CON_BUS_CLEAR, &s_i2c.regs->ic_con);
    i2c_writel(IC_RX_TL, &s_i2c.regs->ic_rx_tl);
    i2c_writel(IC_TX_TL, &s_i2c.regs->ic_tx_tl);
    drv_i2c_hw_set_speed(100000);
    i2c_writel(addr, &s_i2c.regs->ic_sar);
    drv_i2c_hw_set_intr_mask(1);

    return drv_i2c_hw_enable(1);
}

/* START / mid-xfer reset: keep prepared response for following Mem_Read */
static void drv_i2c_mcu_reset_xfer(void)
{
    s_i2c.slave.protocol_state = MCU_I2C_STATE_IDLE;
    s_i2c.slave.current_reg = MCU_I2C_REG_CMD_DATA;
    s_i2c.slave.received_cmd = 0;
    s_i2c.slave.data_send_idx = 0;
    s_i2c.slave.write_byte_idx = 0;
    s_i2c.slave.rx_frame_len = 0;
    s_i2c.slave.saw_read = 0;
    s_i2c.slave.saw_write = 0;
}

static void drv_i2c_mcu_reset_state(void)
{
    drv_i2c_mcu_reset_xfer();
    s_i2c.slave.prepared_data_len = 0;
}

void drv_i2c_mcu_reset(void)
{
    drv_i2c_mcu_reset_state();
}

uint8_t drv_i2c_mcu_get_addr(void)
{
    return s_i2c.chip_addr;
}

int drv_i2c_mcu_prepare_error(uint16_t error_data)
{
    s_i2c.slave.prepared_data[0] = (uint8_t)(error_data & 0xFFU);
    s_i2c.slave.prepared_data[1] = (uint8_t)((error_data >> 8) & 0xFFU);
    s_i2c.slave.prepared_data_len = 2;
    s_i2c.slave.data_send_idx = 0;
    s_i2c.slave.protocol_state = MCU_I2C_STATE_DATA_READY;
    return 0;
}

int drv_i2c_mcu_prepare_log(void)
{
    biz_log_buffer_t *logbuf = &g_log_buffer;
    uint32_t copy_len;

    if (logbuf->header.magic != BIZ_LOG_MAGIC)
        return -1;

    copy_len = logbuf->header.write_offset;
    if (copy_len > sizeof(logbuf->data))
        copy_len = sizeof(logbuf->data);
    if (copy_len > MCU_I2C_MAX_DATA_LEN)
        copy_len = MCU_I2C_MAX_DATA_LEN;

    if (copy_len > 0)
        memcpy(s_i2c.slave.prepared_data, logbuf->data, copy_len);
    else
        copy_len = (uint32_t)rt_snprintf((char *)s_i2c.slave.prepared_data,
                                         MCU_I2C_MAX_DATA_LEN, "KA200 log ready");

    s_i2c.slave.prepared_data_len = (uint16_t)copy_len;
    s_i2c.slave.data_send_idx = 0;
    s_i2c.slave.protocol_state = MCU_I2C_STATE_DATA_READY;
    return 0;
}

static void drv_i2c_mcu_try_finish_mailbox(void)
{
    drv_i2c_slave_data_t *sd = &s_i2c.slave;
    uint16_t need;
    int ret;

    if (sd->current_reg != MCU_I2C_REG_CMD_DATA || sd->rx_frame_len == 0)
        return;

    /* Legacy: single-byte READ_LOG command */
    if (sd->rx_frame_len == 1 && sd->rx_frame[0] == MCU_I2C_CMD_READ_LOG)
    {
        sd->received_cmd = MCU_I2C_CMD_READ_LOG;
        sd->protocol_state = MCU_I2C_STATE_CMD_RECEIVED;
        drv_i2c_mcu_prepare_log();
        sd->rx_frame_len = 0;
        return;
    }

    /* Mailbox frame: hdr(4) + payload[len] */
    if (sd->rx_frame_len < BIZ_I2C_CMD_HEADER_LEN)
        return;

    need = (uint16_t)(BIZ_I2C_CMD_HEADER_LEN + sd->rx_frame[1]);
    if (sd->rx_frame_len < need)
        return;

    ret = biz_i2c_proxy_handle(sd->rx_frame, need,
                               sd->prepared_data, MCU_I2C_MAX_DATA_LEN);
    if (ret > 0)
    {
        sd->prepared_data_len = (uint16_t)ret;
        sd->data_send_idx = 0;
        sd->protocol_state = MCU_I2C_STATE_DATA_READY;
        sd->rx_frame_len = 0;
    }
    else if (ret == 0)
    {
        /* Push / write-only — no response expected */
        sd->prepared_data_len = 0;
        sd->protocol_state = MCU_I2C_STATE_IDLE;
        sd->rx_frame_len = 0;
    }
    else
    {
        BIZ_WARN("I2C mailbox cmd 0x%02x failed\n", sd->rx_frame[0]);
        sd->prepared_data_len = 0;
        sd->rx_frame_len = 0;
    }
}

static int drv_i2c_mcu_write_byte(uint8_t data)
{
    drv_i2c_slave_data_t *sd = &s_i2c.slave;

    if (sd->write_byte_idx == 0)
    {
        sd->current_reg = data;
        sd->rx_frame_len = 0;
        /* New Host write: drop stale rsp unless this is Mem_Read reg-select only */
        if (data != MCU_I2C_REG_CMD_DATA && data != MCU_I2C_REG_DATA)
            return -1;
        if (data == MCU_I2C_REG_DATA && sd->prepared_data_len == 0)
            drv_i2c_mcu_prepare_log();
    }
    else
    {
        sd->saw_write = 1;
        if (sd->current_reg == MCU_I2C_REG_CMD_DATA)
        {
            if (sd->rx_frame_len < MCU_I2C_MAX_DATA_LEN)
                sd->rx_frame[sd->rx_frame_len++] = data;
            /*
             * Do NOT finish mailbox here. OTA DATA memcpy (and other handlers)
             * used to run mid-RX while MCU master still clocks → long SCL
             * stretch → STM32 Mem_Write can hang past Host ACK timeout.
             * Finish only on STOP (see DRV_I2C_SLAVE_STOP).
             */
        }
    }

    sd->write_byte_idx++;
    return 0;
}

static void drv_i2c_mcu_fill_tx_fifo(void)
{
    drv_i2c_slave_data_t *sd = &s_i2c.slave;
    uint32_t n = 0;

    sd->saw_read = 1;

    if (sd->prepared_data_len == 0)
    {
        i2c_writel(0x00, &s_i2c.regs->ic_cmd_data);
        i2c_writel(0x00, &s_i2c.regs->ic_cmd_data);
        return;
    }

    while (n < MCU_I2C_TX_FIFO_BURST && sd->data_send_idx < sd->prepared_data_len)
    {
        i2c_writel(sd->prepared_data[sd->data_send_idx], &s_i2c.regs->ic_cmd_data);
        sd->data_send_idx++;
        n++;
    }

    /* Keep SCL moving if Host asked for more than we have */
    if (n == 0)
        i2c_writel(0x00, &s_i2c.regs->ic_cmd_data);
}

static int drv_i2c_mcu_slave_cb(drv_i2c_slave_event_t event, uint8_t *val)
{
    if (!s_i2c.initialized || !s_i2c.slave_enabled)
        return -1;

    switch (event)
    {
    case DRV_I2C_SLAVE_WRITE_REQUESTED:
        s_i2c.slave.write_byte_idx = 0;
        s_i2c.slave.current_reg = 0xFFU;
        s_i2c.slave.rx_frame_len = 0;
        s_i2c.slave.saw_write = 0;
        s_i2c.slave.saw_read = 0;
        break;
    case DRV_I2C_SLAVE_WRITE_RECEIVED:
        if (val)
            return drv_i2c_mcu_write_byte(*val);
        break;
    case DRV_I2C_SLAVE_READ_REQUESTED:
    case DRV_I2C_SLAVE_READ_PROCESSED:
        break;
    case DRV_I2C_SLAVE_STOP:
        /* Write-only: finish short/legacy cmds that waited for STOP */
        if (s_i2c.slave.saw_write && !s_i2c.slave.saw_read)
            drv_i2c_mcu_try_finish_mailbox();

        /*
         * Preserve mailbox rsp across Write→STOP→Read (MCU Delay + Mem_Read).
         * Clear only after a read completed, or after a write that left no rsp.
         */
        if (s_i2c.slave.saw_read ||
            (s_i2c.slave.saw_write && s_i2c.slave.prepared_data_len == 0))
        {
            drv_i2c_mcu_reset_state();
        }
        else
        {
            drv_i2c_mcu_reset_xfer();
            if (s_i2c.slave.prepared_data_len > 0)
                s_i2c.slave.protocol_state = MCU_I2C_STATE_DATA_READY;
        }
        break;
    default:
        break;
    }

    return 0;
}

static void drv_i2c_config_irq_group(void)
{
    rt_ubase_t dist_base = platform_get_gic_dist_base();
    volatile rt_uint32_t *gicd_igroupr;
    volatile rt_uint32_t *gicd_igrpmodr;
    rt_uint32_t reg_index = I2C0_IRQ_NUM / 32;
    rt_uint32_t bit_index = I2C0_IRQ_NUM % 32;
    rt_uint32_t mask = (1U << bit_index);

    gicd_igroupr = (volatile rt_uint32_t *)(dist_base + GICD_IGROUPR_OFFSET + reg_index * 4);
    gicd_igrpmodr = (volatile rt_uint32_t *)(dist_base + GICD_IGRPMODR_OFFSET + reg_index * 4);

    *gicd_igroupr |= mask;
    *gicd_igrpmodr &= ~mask;
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}

static int drv_i2c_is_mcu_bus(void)
{
    return s_i2c.regs == (void *)(uintptr_t)DRV_I2C0_BASE;
}

/* Returns: 1=STOP done, 0=continue, -1=error */
static int drv_i2c_irq_process(void)
{
    uint32_t cntl;
    uint32_t temp;
    uint32_t rx_data;
    int ret = 0;
    int is_mcu = drv_i2c_is_mcu_bus();

    cntl = i2c_readl(&s_i2c.regs->ic_raw_intr_stat);
    cntl &= ~(IC_ACTIVITY | IC_TX_EMPTY);

    if (cntl == 0)
        return 0;

    if (cntl & IC_START_DET)
    {
        i2c_readl(&s_i2c.regs->ic_clr_start_det);
        if (is_mcu)
        {
            /* Preserve prepared_data for Write→Mem_Read turnaround */
            drv_i2c_mcu_reset_xfer();
            s_i2c.slave.current_reg = MCU_I2C_REG_CMD_DATA;
            drv_i2c_mcu_slave_cb(DRV_I2C_SLAVE_WRITE_REQUESTED, RT_NULL);
        }
    }

    if (cntl & IC_RESTART_DET)
        i2c_readl(&s_i2c.regs->ic_clr_start_det);

    if (cntl & IC_RX_FULL)
    {
        rx_data = i2c_readl(&s_i2c.regs->ic_cmd_data);
        if (is_mcu)
        {
            uint8_t data = (uint8_t)rx_data;
            drv_i2c_mcu_slave_cb(DRV_I2C_SLAVE_WRITE_RECEIVED, &data);
        }
    }

    if (cntl & IC_RD_REQ)
    {
        if (is_mcu)
            drv_i2c_mcu_fill_tx_fifo();
        i2c_readl(&s_i2c.regs->ic_clr_rd_req);
    }

    if (cntl & IC_RX_DONE)
        i2c_readl(&s_i2c.regs->ic_clr_rx_done);

    if (cntl & IC_GEN_CALL)
        i2c_readl(&s_i2c.regs->ic_clr_gen_call);

    if (cntl & IC_STOP_DET)
    {
        i2c_readl(&s_i2c.regs->ic_clr_stop_det);
        if (is_mcu)
            drv_i2c_mcu_slave_cb(DRV_I2C_SLAVE_STOP, RT_NULL);
        return 1;
    }

    if (cntl & IC_SCL_STUCKLOW)
    {
        *(volatile uint32_t *)0x12500094U = 0x38U;
        *(volatile uint32_t *)0x12500094U = 0x3FU;
    }

    if (cntl & IC_TX_ABRT)
    {
        uint32_t abrt = i2c_readl(&s_i2c.regs->ic_tx_abrt_source);

        if (abrt & IC_TX_ABRT_SDA_STUCK)
        {
            int wait = 100;

            temp = i2c_readl(&s_i2c.regs->ic_enable);
            temp |= IC_ENABLE_SDA_STUCK;
            i2c_writel(temp, &s_i2c.regs->ic_enable);
            /* Never busy-wait forever — that used to hard-lock CPU0. */
            while (wait-- > 0 &&
                   !(i2c_readl(&s_i2c.regs->ic_enable) & IC_ENABLE_SDA_STUCK))
                ;
            if (i2c_readl(&s_i2c.regs->ic_status) & IC_STATUS_SDA_STUCK_NOT_OK)
            {
                *(volatile uint32_t *)0x12500094U = 0x38U;
                *(volatile uint32_t *)0x12500094U = 0x3FU;
            }
        }
        /* Always clear ABRT so the IRQ line can drop. */
        i2c_readl(&s_i2c.regs->ic_clr_tx_abrt);
        ret = -1;
    }

    return ret;
}

static void drv_i2c_isr(int vector, void *param)
{
    (void)vector;
    (void)param;

    rt_interrupt_enter();
    /*
     * Mask DW IRQs until BH drains IC_RAW_INTR_STAT. Leaving them unmasked
     * while only posting a semaphore lets GIC re-enter forever during MCU
     * i2c_scan (START/STOP barrage) and stars out msh on CPU0.
     */
    if (s_i2c.regs)
        i2c_writel(0, &s_i2c.regs->ic_intr_mask);
    rt_sem_release(&s_i2c_irq_sem);
    rt_interrupt_leave();
}

void drv_i2c_bh_entry(void *param)
{
    (void)param;

    while (1)
    {
        rt_sem_take(&s_i2c_irq_sem, RT_WAITING_FOREVER);
        biz_mcu_err_drain();

        for (;;)
        {
            int st = drv_i2c_irq_process();
            if (st != 0)
                break;
            if ((i2c_readl(&s_i2c.regs->ic_raw_intr_stat) &
                 ~(IC_ACTIVITY | IC_TX_EMPTY)) == 0)
                break;
        }
        /* Re-arm after pending bits are cleared. */
        drv_i2c_hw_set_intr_mask(1);
    }
}

int drv_i2c_init(void)
{
    int ret;

    if (s_i2c.initialized)
        return BIZ_SUCCESS;

    rt_memset(&s_i2c, 0, sizeof(s_i2c));
    rt_sem_init(&s_i2c_irq_sem, "i2c_irq", 0, RT_IPC_FLAG_FIFO);
    s_i2c.chip_addr = (uint8_t)drv_i2c_mcu_resolve_addr();

    ret = drv_i2c_hw_init_slave(s_i2c.chip_addr);
    if (ret != 0)
    {
        BIZ_WARN("I2C0 slave init failed\n");
        return BIZ_ERR_NORMAL;
    }

    drv_i2c_config_irq_group();
    rt_hw_interrupt_install(I2C0_IRQ_NUM, drv_i2c_isr, RT_NULL, "i2c0");
    rt_hw_interrupt_umask(I2C0_IRQ_NUM);
    s_i2c.irq_installed = 1;

    drv_i2c_mcu_reset_state();
    s_i2c.initialized = 1;
    s_i2c.slave_enabled = 1;

    BIZ_INFO("I2C MCU slave ready @ 0x%02x IRQ%d\n", s_i2c.chip_addr, I2C0_IRQ_NUM);
    return BIZ_SUCCESS;
}

int drv_i2c_is_ready(void)
{
    return s_i2c.initialized ? 1 : 0;
}
