/*
 * drv_emmc_core.c — eMMC hardware layer (DesignWare SDHCI + ADMA3).
 * Polled from emmc_biz thread (hp640 SPL equivalent).
 */

#include "drv_emmc.h"
#include "biz_log.h"
#include "biz_ipc.h"
#include "drv_efuse.h"
#include "board.h"
#include "tick.h"
#include <rthw.h>
#include <rtthread.h>
#include <string.h>

#define EMMC_BASE               CONFIG_EMMC_BASE_ADDR

/* RTT tuning: reject premature tap lock; see doc/EMMC_TUNING.md */
#define EMMC_TUNING_TAP_MIN         0x34U
#define EMMC_TUNING_LATCH_MIN_ITER  63

/* Runtime level: HP_LOGI / HP_LOGD via biz_log (msh: log info|debug) */
#define EMMC_BOOT_LOG(fmt, ...)     HP_LOGI("[drv] emmc: " fmt, ##__VA_ARGS__)
#ifdef BSP_DRV_EMMC_DEBUG
#define EMMC_DBG_LOG(fmt, ...)      HP_LOGD("[drv] emmc: " fmt, ##__VA_ARGS__)
#define EMMC_REG_DUMP(tag)          emmc_dump_regs(tag)
#else
#define EMMC_DBG_LOG(fmt, ...)      ((void)0)
#define EMMC_REG_DUMP(tag)          ((void)0)
#endif

/* SDHCI registers (byte offsets, sdhci.h) */
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0C
#define SDHCI_COMMAND           0x0E
#define SDHCI_RESPONSE          0x10
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_POWER_CONTROL     0x29
#define SDHCI_CLOCK_CONTROL     0x2C
#define SDHCI_TIMEOUT_CONTROL   0x2E
#define SDHCI_SOFTWARE_RESET    0x2F
#define SDHCI_INT_STATUS        0x30
#define SDHCI_ADMA_ERROR        0x54
#define SDHCI_ADMA_ID_LOW       0x78
#define SDHCI_ADMA_ID_HI        0x7C

#define SDHCI_TRNS_DMA          (1U << 0)
#define SDHCI_TRNS_BLK_CNT_EN   (1U << 1)
#define SDHCI_TRNS_ACMD23       (1U << 3)
#define SDHCI_TRNS_READ         (1U << 4)
#define SDHCI_TRNS_MULTI        (1U << 5)

#define SDHCI_CMD_CRC           0x08
#define SDHCI_CMD_INDEX         0x10
#define SDHCI_CMD_DATA          0x20
#define SDHCI_CMD_RESP_NONE     0x00
#define SDHCI_CMD_RESP_LONG     0x01
#define SDHCI_CMD_RESP_SHORT    0x02
#define SDHCI_CMD_RESP_SHORT_BUSY 0x03
#define SDHCI_MAKE_CMD(c, f)    (((c & 0xff) << 8) | (f & 0xff))
#define SDHCI_MAKE_BLKSZ(dma, blksz) (((dma & 0x7) << 12) | (blksz & 0xFFF))

#define SDHCI_CMD_INHIBIT       (1U << 0)
#define SDHCI_DATA_INHIBIT      (1U << 1)
#define SDHCI_INT_RESPONSE      (1U << 0)
#define SDHCI_INT_DATA_END      (1U << 1)
#define SDHCI_INT_DMA_END       (1U << 3)
#define SDHCI_INT_SPACE_AVAIL   (1U << 4)
#define SDHCI_INT_DATA_AVAIL    (1U << 5)
#define SDHCI_INT_ERROR         (1U << 15)
#define SDHCI_INT_TIMEOUT       (1U << 16)
#define SDHCI_INT_CRC           (1U << 17)
#define SDHCI_INT_END_BIT       (1U << 18)
#define SDHCI_INT_INDEX         (1U << 19)
#define SDHCI_INT_DATA_TIMEOUT  (1U << 20)
#define SDHCI_INT_DATA_CRC      (1U << 21)
#define SDHCI_INT_DATA_END_BIT  (1U << 22)
#define SDHCI_INT_ADMA_ERROR    (1U << 25)
#define SDHCI_INT_ALL_MASK      0xFFFFFFFFU

#define SDHCI_CTRL_DMA_MASK     0x18
#define SDHCI_CTRL_SEL_ADMA2    0x10
#define SDHCI_CTRL_SEL_ADMA3    0x18
#define SDHCI_CTRL_SEL_ADMA2_ADMA3 0x18
#define SDHCI_POWER_ON          0x01
#define SDHCI_POWER_180         0x0A
#define SDHCI_POWER_330         0x0E
#define SDHCI_CLOCK_INT_EN      (1U << 0)
#define SDHCI_CLOCK_INT_STABLE  (1U << 1)
#define SDHCI_CLOCK_CARD_EN     (1U << 2)
#define SDHCI_RESET_ALL         0x01
#define SDHCI_RESET_CMD         0x02
#define SDHCI_RESET_DATA        0x04
#define SDHCI_DATA_TIMEOUT      0x0A
#define SDHCI_DEFAULT_BOUNDARY  0x07
#define SDHCI_BUFFER            0x20
#define AT_STAT_VALID_MASK      0xffU
#define SDHCI_HOST_CONTROL2     0x3E
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_CLOCK_BASE_MASK   0x00003F00U
#define SDHCI_CLOCK_V3_BASE_MASK 0x0000FF00U
#define SDHCI_CLOCK_BASE_SHIFT  8
#define SDHCI_INT_ENABLE        0x34
/* hp640 include/sdhci.h — enable cmd + data + error bits in INT_ENABLE */
#define SDHCI_INT_CMD_MASK      (SDHCI_INT_RESPONSE | SDHCI_INT_TIMEOUT | \
                                 SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)
#define SDHCI_INT_DATA_MASK     (SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | \
                                 SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL | \
                                 SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC | \
                                 SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)
#define SDHCI_INT_XFER_DONE     (SDHCI_INT_DATA_END | SDHCI_INT_DATA_END_BIT)

#define SDHCI_CTRL_UHS_MASK     0x0007
#define SDHCI_CTRL_SPEED_MASK   0x0007
#define SDHCI_CTRL_EMMC_LEGACY  0x0000
#define SDHCI_CTRL_EMMC_Legacy  0x0000
#define SDHCI_CTRL_EMMC_HS_200  0x0003
#define SDHCI_CTRL_EMMC_HS_400  0x0007
#define SDHCI_CTRL_VDD_180      0x0008
#define SDHCI_CTRL_CMD23_ENABLE 0x0800
#define SDHCI_CTRL_HOST_VER4_ENABLE 0x1000
#define SDHCI_CTRL_64BIT_ADDRESS    0x2000
#define SDHCI_CTRL_HISPD        0x04
#define SDHCI_DIV_MASK          0xFF
#define SDHCI_DIVIDER_SHIFT     8
#define SDHCI_DIVIDER_HI_SHIFT  6
#define SDHCI_DIV_HI_MASK       0x300U
#define SDHCI_DIV_MASK_LEN      8
#define SDHCI_PLL_ENABLE        (1U << 3)
#define SDHCI_HOST_VERSION      0xFE
#define SDHCI_SPEC_300          2
#define SDHCI_MAX_DIV_SPEC_300  2046U
#define EMMC_VENDOR_OFFSET_DFLT 0x500U
#define EMMC_CTRL_OFF           0x2cU
#define MSHC_CTRL_R_OFF         0x08U
#define MBIU_CTRL_R_OFF         0x10U
#define AUTO_TUNING_CTRL_OFF    0x40U
#define AUTO_TUNING_STAT_OFF    0x44U
#define CARD_IS_EMMC            (1U << 0)
#define ENH_STROBE_ENABLE       (1U << 8)
#define LYNCHIP_CPR               0x12500000UL
#define CPR_EMMC_CTRL             (LYNCHIP_CPR + 0x88UL)

#define EMMC_PHY_OFFSET         0x300U
#define PHY_CFG                 0x00U
#define CMD_PAD_CFG             0x04U
#define DATA_PAD_CFG            0x06U
#define CLK_PAD_CFG             0x08U
#define STB_PAD_CFG             0x0aU
#define RST_PAD_CFG             0x0cU
#define SDCLKDL_CFG             0x1dU
#define SDCLK_DC                0x1eU
#define SMPLDL_CFG              0x20U
#define ATDL_CFG                0x21U
#define DLL_CTRL                0x24U
#define DLL_CNFG1               0x25U
#define DLL_CNFG2               0x26U
#define DLLDL_CNFG              0x28U
#define DLL_OFFST               0x29U
#define DLLLBT_CNFG             0x2cU
#define DLL_STATUS              0x2eU
#define DLL_IS_LOCKED           (1U << 0)
#define DLL_ERROR_STS           (1U << 1)

#define EMMC_HOST_MAX_CLK_HZ    100000000U
#define EMMC_HS200_CLK_HZ       200000000U
#define EMMC_HS400_CLK_HZ       200000000U
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06
#define SDHCI_CTRL_EMMC_HS_SDR  0x0001
#define SDHCI_CTRL_EXEC_TUNING  0x0040
#define SDHCI_CTRL_TUNED_CLK    0x0080
#define SDHCI_READ_STATUS_TIMEOUT 1000
#define SDHCI_TUNING_LOOP_COUNT 128

#define P_VENDOR1_SPECIFIC_AREA 0xe8

#define MMC_CMD_SEND_TUNING_BLOCK_HS200 21

#define EXT_CSD_TIMING_HS       1U
#define EXT_CSD_CMD_SET_NORMAL  0U

/* spl_cmd.c: HS400_FPGA_MODE — backend is FPGA-instantiated eMMC, strict AT/tuning */
#define EMMC_FPGA_BACKEND           1

#define EMMC_SDCLK_DC_HS400_KA200M  0x21U
#define EMMC_SDCLK_DC_HS400_KA200   0x23U
#define EMMC_SDCLK_DC_HS400_CUSTOM  0x3cU
#define EMMC_MODE_LEGACY        0
#define EMMC_MODE_HS            1
#define EMMC_MODE_HS200         2
#define EMMC_MODE_HS400         3

#define EMMC_LEGACY_SPEED_HZ    26000000U

#define EXT_CSD_BUS_WIDTH       183U
#define EXT_CSD_HS_TIMING       185U
#define EXT_CSD_BUS_WIDTH_8     2U
#define EXT_CSD_TIMING_HS200    2U
#define EXT_CSD_TIMING_HS400    3U
#define EXT_CSD_DDR_FLAG        4U
#define MMC_SWITCH_MODE_WRITE_BYTE 0U

#define CMD_DESC_LINK_VALID     0x09U
#define INTERG_DESC_LINK_VALID  0x39U
#define INTERG_DESC_END_VALID   0x3BU
#define ADMA_DESC_ATTR_VALID    (1U << 0)
#define ADMA_DESC_ATTR_END      (1U << 1)
#define ADMA_DESC_TRANSFER_DATA (1U << 5)

#define MMC_CMD_GO_IDLE_STATE       0
#define MMC_CMD_SEND_OP_COND        1
#define MMC_CMD_SET_BLOCKLEN        16
#define MMC_CMD_SWITCH              6
#define MMC_CMD_READ_SINGLE_BLOCK   17
#define MMC_CMD_READ_MULTIPLE_BLOCK 18
#define MMC_CMD_WRITE_SINGLE_BLOCK  24
#define MMC_CMD_WRITE_MULTIPLE_BLOCK 25

#define ADMA_MAX_LEN            (64U * 512U)
#define SDHCI_CMD_DEFAULT_TIMEOUT 100

typedef struct {
    uint32_t attr;
    uint32_t content;
} adma_cmd_desc_item_t;

typedef struct __attribute__((packed)) {
    uint16_t attr;
    uint16_t len;
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t rsvd;
} adma_desc_t;

typedef struct __attribute__((packed)) {
    adma_cmd_desc_item_t cmd_desc[4];
    adma_desc_t adma_desc[EMMC_ADMA_DESC_LEN];
} adma_desc_pair_t;

typedef struct __attribute__((packed)) {
    uint32_t attr;
    uint32_t pointer_addr_low;
    uint32_t pointer_addr_hi;
    uint32_t rsvd2;
} integrated_desc_t;

emmc_statistics_t s_emmc_stats __attribute__((section(".bss.drv_emmc_stats")));

/* hp640 spl_cmd.c: interg/pair/heart_beat_buf in SPL BSS */
static integrated_desc_t s_interg_base[EMMC_MAX_ADMA_LINK] HP232X_DMA_BUF_ATTR;
static adma_desc_pair_t s_pair_base[EMMC_MAX_ADMA_LINK] HP232X_DMA_BUF_ATTR;
static uint8_t s_heartbeat_buf[EMMC_BLK_SIZE] HP232X_DMA_BUF_ATTR;
static uint8_t s_query_buf[EMMC_BLK_SIZE] HP232X_DMA_BUF_ATTR;

static inline integrated_desc_t *emmc_dma_interg(void)
{
    return s_interg_base;
}

static inline adma_desc_pair_t *emmc_dma_pair(void)
{
    return s_pair_base;
}
static uint8_t s_emmc_initialized;
static uint32_t s_emmc_error_mask;

typedef struct {
    uint8_t init_in_progress;
    uint8_t bus_width;
    uint8_t selected_mode;
    uint8_t enhace_strobe;
    uint8_t ddr_mode;
    uint16_t read_bl_len;
    uint32_t tran_speed;
    uint32_t legacy_speed;
} emmc_host_state_t;

static emmc_host_state_t s_host;
static uint16_t s_emmc_offset = EMMC_VENDOR_OFFSET_DFLT;
static uint16_t s_host_version;
static uint8_t s_v4_enable;
static int8_t s_emmc_chip_type = -1; /* DRV_CHIP_KA200 / DRV_CHIP_KA200M, -1=unset */

/* hp640 check_chip_type() via efuse BLK44/45 */
static int emmc_chip_type_get(void)
{
    if (s_emmc_chip_type < 0)
        s_emmc_chip_type = (int8_t)drv_efuse_check_chip_type();
    return (int)s_emmc_chip_type;
}

/* hp640 snps_sdhci_set_tapdelay() HS400 SDCLK_DC branch */
static uint8_t emmc_hs400_sdclk_dc(void)
{
#ifdef BSP_EMMC_CUSTOM_DC
    return EMMC_SDCLK_DC_HS400_CUSTOM;
#else
    if (emmc_chip_type_get() == DRV_CHIP_KA200M)
        return EMMC_SDCLK_DC_HS400_KA200M;
    return EMMC_SDCLK_DC_HS400_KA200;
#endif
}

static inline uint32_t emmc_vendor_reg(uint32_t off)
{
    return s_emmc_offset + off;
}

static uint32_t emmc_mode_freq(uint8_t mode)
{
    static const uint32_t freqs[] = {
        [EMMC_MODE_LEGACY] = 25000000U,
        [EMMC_MODE_HS]     = EMMC_LEGACY_SPEED_HZ,
        [EMMC_MODE_HS200]  = EMMC_HS200_CLK_HZ,
        [EMMC_MODE_HS400]  = EMMC_HS400_CLK_HZ,
    };

    if (mode >= sizeof(freqs) / sizeof(freqs[0]))
        return EMMC_LEGACY_SPEED_HZ;
    return freqs[mode];
}

static void emmc_select_mode(uint8_t mode)
{
    s_host.selected_mode = mode;
    s_host.tran_speed = emmc_mode_freq(mode);
    /* hp640: mmc->enhace_strobe only for HS400ES; ENH_STROBE reg bit is separate */
    s_host.enhace_strobe = 0;
    s_host.ddr_mode = (mode == EMMC_MODE_HS400) ? 1U : 0U;
}

static int emmc_set_power_180(void);
static int emmc_set_card_clock(uint32_t hz);
static void emmc_dump_regs(const char *tag);
static uint32_t emmc_at_stat_read(void);
static int emmc_send_cmd_raw(uint16_t cmdidx, uint32_t cmdarg, uint8_t resp_type);
static int emmc_send_cmd(uint16_t cmdidx, uint32_t cmdarg, uint8_t resp_type);
static void emmc_soc_clk_enable(void);
static void emmc_snps_set_ios_post(void);
static int emmc_wait_interrupt(void);

static int emmc_set_clk_hz(uint32_t clock, int disable)
{
    if (disable)
        return emmc_set_card_clock(0);

    if (clock > EMMC_HS400_CLK_HZ)
        clock = EMMC_HS400_CLK_HZ;

    return emmc_set_card_clock(clock);
}

/* hp640 spl_cmd.c _send_cmd() response mapping (resp_type 0xFF00 auto) */
static uint8_t emmc_resp_type_for_cmd(uint16_t cmdidx)
{
    switch (cmdidx)
    {
    case 0:
    case 4:
        return 0;
    case 1:
        return 4; /* R3 — no CRC */
    case 2:
    case 9:
    case 10:
        return 2; /* R2 */
    case 6:
    case 7:
        return 3; /* R1b */
    default:
        return 1; /* R1 */
    }
}

static int emmc_send_cmd_idx(uint16_t cmdidx, uint32_t cmdarg)
{
    return emmc_send_cmd(cmdidx, cmdarg, emmc_resp_type_for_cmd(cmdidx));
}

static inline uint32_t emmc_readl(uint32_t reg)
{
    return *(volatile uint32_t *)(EMMC_BASE + reg);
}

static inline void emmc_writel(uint32_t val, uint32_t reg)
{
    *(volatile uint32_t *)(EMMC_BASE + reg) = val;
}

static inline uint16_t emmc_readw(uint32_t reg)
{
    return *(volatile uint16_t *)(EMMC_BASE + reg);
}

static inline void emmc_writew(uint16_t val, uint32_t reg)
{
    *(volatile uint16_t *)(EMMC_BASE + reg) = val;
}

static inline uint8_t emmc_readb(uint32_t reg)
{
    return *(volatile uint8_t *)(EMMC_BASE + reg);
}

static inline void emmc_writeb(uint8_t val, uint32_t reg)
{
    *(volatile uint8_t *)(EMMC_BASE + reg) = val;
}

/* hp640 sdhci_cmd_done(): drain RESPONSE for R1/R2 */
static void emmc_read_response(uint8_t resp_type)
{
    int i;

    if (resp_type == 2)
    {
        for (i = 0; i < 4; i++)
        {
            (void)emmc_readl(SDHCI_RESPONSE + (uint32_t)(3 - i) * 4U);
            (void)emmc_readb(SDHCI_RESPONSE + (uint32_t)(3 - i) * 4U - 1U);
        }
    }
    else if (resp_type != 0)
    {
        (void)emmc_readl(SDHCI_RESPONSE);
    }
}

static int emmc_map_cmd_error(uint32_t stat)
{
    if (stat & SDHCI_INT_TIMEOUT)
        return BIZ_ERR_EMMC_CMD_TIMEOUT;
    if (stat & SDHCI_INT_CRC)
        return BIZ_ERR_EMMC_CMD_CRC;
    if (stat & SDHCI_INT_END_BIT)
        return BIZ_ERR_EMMC_CMD_CRC;
    if (stat & SDHCI_INT_INDEX)
        return BIZ_ERR_EMMC_CMD_CRC;
    if (stat & SDHCI_INT_DATA_TIMEOUT)
        return BIZ_ERR_EMMC_DATA_TIMEOUT;
    if (stat & SDHCI_INT_DATA_CRC)
        return BIZ_ERR_EMMC_DATA_CRC;
    if (stat & SDHCI_INT_ADMA_ERROR)
        return BIZ_ERR_EMMC_DATA_TIMEOUT;
    return BIZ_ERR_EMMC_INIT;
}

static int emmc_send_cmd_idx_retry(uint16_t cmdidx, uint32_t cmdarg, int retries)
{
    int ret = BIZ_ERR_EMMC_INIT;
    int i;

    for (i = 0; i <= retries; i++)
    {
        ret = emmc_send_cmd_idx(cmdidx, cmdarg);
        if (ret == BIZ_SUCCESS)
            return ret;
        if (i < retries)
            rt_hw_us_delay(1000);
    }

    HP_LOGI("[drv] emmc CMD%d fail (%d) after %d retries\n",
               cmdidx, ret, retries + 1);
    emmc_dump_regs("cmd_fail");
    return ret;
}

static inline void emmc_phy_writeb(uint32_t off, uint8_t val)
{
    emmc_writeb(val, EMMC_PHY_OFFSET + off);
}

static inline void emmc_phy_writew(uint32_t off, uint16_t val)
{
    emmc_writew(val, EMMC_PHY_OFFSET + off);
}

static inline void emmc_phy_writel(uint32_t off, uint32_t val)
{
    emmc_writel(val, EMMC_PHY_OFFSET + off);
}

static inline uint8_t emmc_phy_readb(uint32_t off)
{
    return emmc_readb(EMMC_PHY_OFFSET + off);
}

static inline uint16_t emmc_phy_readw(uint32_t off)
{
    return emmc_readw(EMMC_PHY_OFFSET + off);
}

static inline uint32_t emmc_phy_readl(uint32_t off)
{
    return emmc_readl(EMMC_PHY_OFFSET + off);
}

static uint32_t emmc_host_max_clk_hz(void)
{
    uint32_t caps = emmc_readl(SDHCI_CAPABILITIES);

    /* hp640 DT: sdhci-caps-mask=0xff00, sdhci-caps=0xc800 */
    caps &= ~0xFF00U;
    caps |= 0xC800U;

    {
        uint32_t mhz = (caps & SDHCI_CLOCK_V3_BASE_MASK) >> SDHCI_CLOCK_BASE_SHIFT;

        if (mhz == 0)
            mhz = (caps & SDHCI_CLOCK_BASE_MASK) >> SDHCI_CLOCK_BASE_SHIFT;
        if (mhz == 0)
            return EMMC_HOST_MAX_CLK_HZ;
        return mhz * 1000000U;
    }
}

static void emmc_soc_clk_enable(void)
{
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)CPR_EMMC_CTRL;
    uint32_t val = *reg;

    *reg = val | 0x0FU;
    rt_hw_us_delay(100);
}

static void emmc_read_vendor_offset(void)
{
    uint16_t off = emmc_readw(P_VENDOR1_SPECIFIC_AREA);

    if (off != 0)
        s_emmc_offset = off;
}

static void emmc_cache_host_version(void)
{
    /* hp640 sdhci_setup_cfg + sdhci_init: host->version from SDHCI_HOST_VERSION */
    s_host_version = emmc_readw(SDHCI_HOST_VERSION);
    s_v4_enable = (s_host_version > SDHCI_SPEC_300) ? 1U : 0U;
}

static void emmc_dump_regs(const char *tag)
{
    HP_LOGI("[drv] emmc %s: off=0x%x v4=%u\n", tag, s_emmc_offset, s_v4_enable);
    HP_LOGI("[drv]   PRESENT=0x%x CLK=0x%x PWR=0x%x HC2=0x%x\n",
               emmc_readl(SDHCI_PRESENT_STATE),
               emmc_readw(SDHCI_CLOCK_CONTROL),
               emmc_readb(SDHCI_POWER_CONTROL),
               emmc_readw(SDHCI_HOST_CONTROL2));
    HP_LOGI("[drv]   INT=0x%x INT_EN=0x%x ADMA_ERR=0x%x EMMC_CTRL=0x%x MSHC=0x%x\n",
               emmc_readl(SDHCI_INT_STATUS),
               emmc_readl(SDHCI_INT_ENABLE),
               emmc_readb(SDHCI_ADMA_ERROR),
               emmc_readl(emmc_vendor_reg(EMMC_CTRL_OFF)),
               emmc_readb(emmc_vendor_reg(MSHC_CTRL_R_OFF)));
    HP_LOGI("[drv]   HC=0x%x HC2=0x%x ADMA_ID=0x%08x:%08x interg=0x%llx\n",
               emmc_readb(SDHCI_HOST_CONTROL),
               emmc_readw(SDHCI_HOST_CONTROL2),
               emmc_readl(SDHCI_ADMA_ID_HI),
               emmc_readl(SDHCI_ADMA_ID_LOW),
               (unsigned long long)(uintptr_t)emmc_dma_interg());
    HP_LOGI("[drv]   BLK_SZ=0x%x BLK_CNT=0x%x ARG=0x%x MODE=0x%x CMD=0x%x\n",
               emmc_readw(SDHCI_BLOCK_SIZE),
               emmc_readw(SDHCI_BLOCK_COUNT),
               emmc_readl(SDHCI_ARGUMENT),
               emmc_readw(SDHCI_TRANSFER_MODE),
               emmc_readw(SDHCI_COMMAND));
    {
        adma_desc_pair_t *pair = emmc_dma_pair();
        integrated_desc_t *interg = emmc_dma_interg();

        HP_LOGI("[drv]   interg attr=0x%x ptr=0x%08x:%08x\n",
                   interg[0].attr,
                   interg[0].pointer_addr_hi,
                   interg[0].pointer_addr_low);
        HP_LOGI("[drv]   pair0 cmd[2]=0x%x cmd[3]=0x%x adma0=0x%08x:%08x len=%u\n",
                   pair[0].cmd_desc[2].content,
                   pair[0].cmd_desc[3].content,
                   pair[0].adma_desc[0].addr_hi,
                   pair[0].adma_desc[0].addr_lo,
                   pair[0].adma_desc[0].len);
        HP_LOGI("[drv]   RESP0=0x%x DLL=0x%x\n",
                   emmc_readl(SDHCI_RESPONSE),
                   emmc_phy_readw(DLL_STATUS));
        HP_LOGI("[drv]   MBIU=0x%x AT_CTRL=0x%x AT_STAT=0x%x (tap=0x%x)\n",
                   emmc_readb(emmc_vendor_reg(MBIU_CTRL_R_OFF)),
                   emmc_readl(emmc_vendor_reg(AUTO_TUNING_CTRL_OFF)),
                   emmc_readl(emmc_vendor_reg(AUTO_TUNING_STAT_OFF)),
                   emmc_at_stat_read());
    }
}

/* hp640 snps_sdhci_set_tapdelay() */
static void emmc_set_tapdelay(void)
{
    uint32_t tap_max = 0;
    uint16_t clk;

    switch (s_host.selected_mode)
    {
    case EMMC_MODE_LEGACY:
    case EMMC_MODE_HS:
        tap_max = 0x63U;
        break;
    case EMMC_MODE_HS200:
        tap_max = 0x2bU;
        break;
    case EMMC_MODE_HS400:
        tap_max = emmc_hs400_sdclk_dc();
        break;
    default:
        return;
    }

    clk = emmc_readw(SDHCI_CLOCK_CONTROL);
    clk &= (uint16_t)~SDHCI_CLOCK_CARD_EN;
    emmc_writew(clk, SDHCI_CLOCK_CONTROL);

    if (s_host.enhace_strobe)
    {
        emmc_phy_writeb(SMPLDL_CFG, 0x08);
        emmc_phy_writeb(ATDL_CFG, 0x08);
    }
    else
    {
        emmc_phy_writeb(SMPLDL_CFG, 0x0c);
        emmc_phy_writeb(ATDL_CFG, 0x08);
    }
    emmc_phy_writeb(SDCLKDL_CFG, 0x10);
    emmc_phy_writeb(SDCLK_DC, (uint8_t)tap_max);
    emmc_phy_writeb(SDCLKDL_CFG, 0x00);

    /* hp640 snps_sdhci_set_tapdelay: always restore CARD_EN */
    clk |= SDHCI_CLOCK_CARD_EN;
    emmc_writew(clk, SDHCI_CLOCK_CONTROL);
}

static void emmc_dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t emmc_cntpct(void)
{
    uint64_t cnt;

    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    return cnt;
}

static uint64_t emmc_cntfrq(void)
{
    uint64_t freq;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

static unsigned int emmc_ms_elapsed(uint64_t start)
{
    uint64_t freq = emmc_cntfrq();

    if (freq == 0)
        return 0;
    return (unsigned int)((emmc_cntpct() - start) * 1000ULL / freq);
}

/* ADMA descriptors / DMA arena only (@ IRAM1_DMA_NC_*). Unchanged by host-scratch NC. */
static int emmc_addr_in_dma_nc(const void *addr, rt_size_t size)
{
#ifdef BSP_EMMC_DMA_CACHED_BSS
    (void)addr;
    (void)size;
    return 0;
#else
    rt_ubase_t start = (rt_ubase_t)addr;
    rt_ubase_t end = start + size;

    return (start >= (rt_ubase_t)IRAM1_DMA_NC_START &&
            end <= (rt_ubase_t)IRAM1_DMA_NC_END);
#endif
}

/* Skip dcache ops: DMA arena, or Host NC windows (IRAM0/1 low). */
static int emmc_addr_skip_dcache(const void *addr, rt_size_t size)
{
    if (emmc_addr_in_dma_nc(addr, size))
        return 1;
    return hp232x_addr_is_normal_nc(addr, size);
}

static void emmc_cache_flush(void *addr, rt_size_t size)
{
    if (!addr || size == 0)
        return;

    if (emmc_addr_skip_dcache(addr, size))
        return;

    rt_ubase_t start = (rt_ubase_t)addr & ~(rt_ubase_t)(BIZ_ARCH_DMA_MINALIGN - 1);
    rt_ubase_t end = ((rt_ubase_t)addr + size + BIZ_ARCH_DMA_MINALIGN - 1) &
                     ~(rt_ubase_t)(BIZ_ARCH_DMA_MINALIGN - 1);

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)start, end - start);
    emmc_dsb();
}

void drv_emmc_flush_dma_buf(void *addr, rt_size_t size)
{
    emmc_cache_flush(addr, size);
}

static void emmc_dma_flush_arena(void)
{
#ifdef BSP_EMMC_DMA_CACHED_BSS
    emmc_cache_flush(s_interg_base, sizeof(s_interg_base));
    emmc_cache_flush(s_pair_base, sizeof(s_pair_base));
#endif
}

uint8_t *drv_emmc_heartbeat_buf(void)
{
    return s_heartbeat_buf;
}

uint8_t *drv_emmc_query_buf(void)
{
    return s_query_buf;
}

/* hp640 emmc_write_data(): write ADMA_ID only (HC2 set once in sdhci_init/reinit) */
static void emmc_kick_adma3(const void *desc)
{
    uint32_t lo = (uint32_t)(uintptr_t)desc;
    uint32_t hi = (uint32_t)((uintptr_t)desc >> 32);

    emmc_writel(lo, SDHCI_ADMA_ID_LOW);
    emmc_writel(hi, SDHCI_ADMA_ID_HI);
}

static void emmc_cache_invalidate(void *addr, rt_size_t size)
{
    if (!addr || size == 0 || emmc_addr_skip_dcache(addr, size))
        return;

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, addr, size);
}

void drv_emmc_report_error(biz_err_code_t err)
{
    if (err == BIZ_ERR_EMMC_STATE_TIMEOUT ||
        err == BIZ_ERR_EMMC_CMD_TIMEOUT ||
        err == BIZ_ERR_EMMC_DATA_TIMEOUT)
    {
        return;
    }

    if (s_emmc_error_mask & (1U << err))
        return;

    s_emmc_error_mask |= (1U << err);
    BIZ_INFO("eMMC error report: code=%d mask=0x%x\n", err, s_emmc_error_mask);
    biz_mcu_err_post(err);
}

static int emmc_wait_inhibit_us(unsigned int max_ms)
{
    unsigned int waited = 0;

    while (emmc_readl(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT))
    {
        if (waited >= max_ms)
            return BIZ_ERR_EMMC_STATE_TIMEOUT;
        rt_hw_us_delay(1000);
        waited++;
    }
    return BIZ_SUCCESS;
}

static void emmc_log_phy_at(const char *tag)
{
    EMMC_DBG_LOG("phy@%s SDCLK_DC=0x%x SMPLDL=0x%x ATDL=0x%x tap=0x%x\n",
                 tag,
                 emmc_phy_readb(SDCLK_DC),
                 emmc_phy_readb(SMPLDL_CFG),
                 emmc_phy_readb(ATDL_CFG),
                 emmc_at_stat_read());
}

static int emmc_wait_bus_idle(int timeout_ms)
{
    return emmc_wait_inhibit_us((unsigned int)timeout_ms);
}

static void emmc_reset(uint8_t mask)
{
    /* hp640 CONFIG_HP640_CUSTOM_EMMC_DELAY: tight poll for FPGA eMMC model */
    unsigned long timeout = 100000UL;

    emmc_writeb(mask, SDHCI_SOFTWARE_RESET);
    while (emmc_readb(SDHCI_SOFTWARE_RESET) & mask)
    {
        if (--timeout == 0)
            break;
    }
}

static int emmc_set_card_clock(uint32_t hz)
{
    uint32_t max_clk = emmc_host_max_clk_hz();
    uint32_t clk = 0;
    unsigned int div = 1;
    int timeout;
    int ret;

    ret = emmc_wait_inhibit_us(100);
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_writew(0, SDHCI_CLOCK_CONTROL);

    if (hz == 0)
        return BIZ_SUCCESS;

    emmc_set_tapdelay();

    if (max_clk <= hz)
        div = 1;
    else
    {
        for (div = 2; div < SDHCI_MAX_DIV_SPEC_300; div += 2U)
        {
            if ((max_clk / div) <= hz)
                break;
        }
    }
    /* hp640 sdhci_set_clock(): div >>= 1; div=0 => full speed (CLK=0xf) */
    div >>= 1;

    clk = (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
    clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN) << SDHCI_DIVIDER_HI_SHIFT;
    clk |= SDHCI_CLOCK_INT_EN;
    emmc_writew((uint16_t)clk, SDHCI_CLOCK_CONTROL);

    timeout = 20;
    while (!(emmc_readw(SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE))
    {
        if (--timeout == 0)
            return BIZ_ERR_EMMC_INIT;
        rt_hw_us_delay(1000);
    }

    if (s_v4_enable)
    {
        clk = emmc_readw(SDHCI_CLOCK_CONTROL);
        clk |= SDHCI_PLL_ENABLE;
        emmc_writew(clk, SDHCI_CLOCK_CONTROL);

        timeout = 1000;
        while (!(emmc_readw(SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE))
        {
            if (--timeout == 0)
                break;
            rt_hw_us_delay(1000);
        }
    }

    clk = emmc_readw(SDHCI_CLOCK_CONTROL);
    clk |= SDHCI_CLOCK_CARD_EN;
    emmc_writew(clk, SDHCI_CLOCK_CONTROL);

    {
        uint8_t ctrl = emmc_readb(SDHCI_HOST_CONTROL);

        if (hz > 26000000U)
            ctrl |= SDHCI_CTRL_HISPD;
        else
            ctrl &= (uint8_t)~SDHCI_CTRL_HISPD;
        emmc_writeb(ctrl, SDHCI_HOST_CONTROL);
    }

    emmc_snps_set_ios_post();
    return BIZ_SUCCESS;
}

/* hp640 CONFIG_LYNCHIP_EVB set_phy() — byte-for-byte sequence */
static int emmc_phy_init(void)
{
    uint32_t state;
    uint16_t clk;
    int timeout;

    clk = emmc_readw(SDHCI_CLOCK_CONTROL);
    clk &= (uint16_t)~SDHCI_CLOCK_INT_EN;
    emmc_writew(clk, SDHCI_CLOCK_CONTROL);

    emmc_phy_writeb(SDCLKDL_CFG, 0x10);
    emmc_phy_writeb(SDCLK_DC, 0x7f);
    emmc_phy_writeb(SDCLKDL_CFG, 0x00);

    clk = emmc_readw(SDHCI_CLOCK_CONTROL);
    clk |= SDHCI_CLOCK_INT_EN;
    emmc_writew(clk, SDHCI_CLOCK_CONTROL);

    emmc_phy_writel(PHY_CFG, 0x00);
    rt_hw_us_delay(10);
    emmc_phy_writel(PHY_CFG, 0x990002);

    emmc_phy_writew(CMD_PAD_CFG, 0x669);
    emmc_phy_writew(DATA_PAD_CFG, 0x669);
    emmc_phy_writew(CLK_PAD_CFG, 0x660);
    emmc_phy_writew(STB_PAD_CFG, 0x671);
    emmc_phy_writew(RST_PAD_CFG, 0x669);
    emmc_phy_writeb(SMPLDL_CFG, 0x0c);

    timeout = 100000;
    do {
        state = emmc_phy_readl(PHY_CFG);
        if (--timeout == 0)
        {
            HP_LOGI("[drv] emmc phy: ready timeout PHY_CFG=0x%x\n", state);
            emmc_dump_regs("phy");
            return BIZ_ERR_EMMC_INIT;
        }
        rt_hw_us_delay(10);
    } while (!(state & 0x02U));

    emmc_phy_writel(PHY_CFG, 0xff0003);
    emmc_writeb(0x11, emmc_vendor_reg(MSHC_CTRL_R_OFF));
    return BIZ_SUCCESS;
}

/* hp640 snps_sdhci_set_ios_post() */
static void emmc_snps_set_ios_post(void)
{
    uint32_t reg = emmc_readl(emmc_vendor_reg(EMMC_CTRL_OFF));

    reg |= CARD_IS_EMMC | ENH_STROBE_ENABLE;
    emmc_writel(reg, emmc_vendor_reg(EMMC_CTRL_OFF));
}

static int emmc_power_off(void)
{
    emmc_set_card_clock(0);
    emmc_writeb(0, SDHCI_POWER_CONTROL);
    return BIZ_SUCCESS;
}

static int emmc_power_on(void)
{
    return emmc_set_power_180();
}

/* hp640 mmc_power_cycle() */
static int emmc_power_cycle(void)
{
    int ret = emmc_power_off();

    if (ret != BIZ_SUCCESS)
        return ret;

    rt_hw_us_delay(2000);
    return emmc_power_on();
}

/* hp640 sdhci_dll_config() — call before writing HOST_CONTROL2 for HS200/HS400 */
static int emmc_dll_config(int enable)
{
    uint16_t clk;
    uint16_t stat;
    int timeout;

    if (!enable)
    {
        emmc_phy_writeb(DLL_CTRL, 0);
        return BIZ_SUCCESS;
    }

    clk = emmc_readw(SDHCI_CLOCK_CONTROL);
    clk &= (uint16_t)~SDHCI_CLOCK_CARD_EN;
    emmc_writew(clk, SDHCI_CLOCK_CONTROL);

    emmc_phy_writeb(SMPLDL_CFG, 0x0c);
    emmc_phy_writeb(ATDL_CFG, 0x08);
    emmc_phy_writeb(DLL_CNFG1, 0x25);
    emmc_phy_writeb(DLL_CNFG2, 0x17);
    emmc_phy_writeb(DLLDL_CNFG, 0x60);
    emmc_phy_writeb(DLL_OFFST, 0x74);
    emmc_phy_writew(DLLLBT_CNFG, 0x2800);

    timeout = 20;
    while (!(emmc_readw(SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE))
    {
        if (--timeout == 0)
            return BIZ_ERR_EMMC_INIT;
        rt_hw_us_delay(1000);
    }

    clk = emmc_readw(SDHCI_CLOCK_CONTROL);
    clk |= SDHCI_CLOCK_CARD_EN;
    emmc_writew(clk, SDHCI_CLOCK_CONTROL);
    emmc_phy_writeb(DLL_CTRL, 0x03);

    timeout = 20;
    while (!((stat = emmc_phy_readw(DLL_STATUS)) & DLL_IS_LOCKED))
    {
        if (--timeout == 0)
        {
            HP_LOGI("[drv] emmc dll: lock timeout stat=0x%x\n", stat);
            return BIZ_ERR_EMMC_INIT;
        }
        rt_hw_us_delay(1000);
    }

    stat = emmc_phy_readw(DLL_STATUS);
    if (stat & DLL_ERROR_STS)
    {
        HP_LOGI("[drv] emmc dll: lock err stat=0x%x\n", stat);
        return BIZ_ERR_EMMC_INIT;
    }

    return BIZ_SUCCESS;
}

static int emmc_dll_config_retry(int enable)
{
    int ret;
    int attempt;

    if (!enable)
        return emmc_dll_config(0);

    for (attempt = 0; attempt < 3; attempt++)
    {
        if (attempt > 0)
        {
            emmc_phy_writeb(DLL_CTRL, 0);
            rt_hw_us_delay(2000);
        }

        ret = emmc_dll_config(1);
        if (ret == BIZ_SUCCESS)
            return ret;

        HP_LOGI("[drv] emmc dll: retry %d stat=0x%x\n",
                   attempt + 1, emmc_phy_readw(DLL_STATUS));
    }

    return ret;
}

/* hp640 sdhci_at_ctr_config() */
static void emmc_at_ctrl_config(void)
{
    emmc_writel(0xf1d0000, emmc_vendor_reg(AUTO_TUNING_CTRL_OFF));
}

static uint32_t emmc_at_stat_read(void)
{
    return emmc_readl(emmc_vendor_reg(AUTO_TUNING_STAT_OFF)) & AT_STAT_VALID_MASK;
}

/* hp640 auto_run: INT_ENABLE already set; re-assert before ADMA kick if needed */
static void emmc_int_enable_adma(void)
{
    emmc_writel(SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK, SDHCI_INT_ENABLE);
    emmc_writel(0, SDHCI_SIGNAL_ENABLE);
}

/* hp640 emmc_write_data(): no AT refresh before xfer; INT_EN set at init only */
static void emmc_prepare_hs400_xfer(void)
{
    if (s_host.selected_mode != EMMC_MODE_HS400)
        return;

    emmc_int_enable_adma();
}

/* hp640 sdhci_set_timing() */
static int emmc_sdhci_set_timing(uint8_t mode, int strict_dll)
{
    uint16_t ctrl2;
    int need_dll = 0;
    int ret;

    ctrl2 = emmc_readw(SDHCI_HOST_CONTROL2);
    ctrl2 &= (uint16_t)~(SDHCI_CTRL_UHS_MASK | SDHCI_CTRL_VDD_180);

    switch (mode)
    {
    case EMMC_MODE_LEGACY:
        ctrl2 |= SDHCI_CTRL_EMMC_LEGACY;
        ctrl2 &= (uint16_t)~SDHCI_CTRL_TUNED_CLK;
        break;
    case EMMC_MODE_HS:
        ctrl2 |= SDHCI_CTRL_EMMC_HS_SDR;
        ctrl2 &= (uint16_t)~SDHCI_CTRL_TUNED_CLK;
        break;
    case EMMC_MODE_HS200:
        ctrl2 |= SDHCI_CTRL_EMMC_HS_200;
        break;
    case EMMC_MODE_HS400:
        ctrl2 |= SDHCI_CTRL_EMMC_HS_400;
        need_dll = 1;
        break;
    default:
        return BIZ_ERR_EMMC_INIT;
    }

    ctrl2 |= SDHCI_CTRL_VDD_180;
    ctrl2 |= SDHCI_CTRL_HOST_VER4_ENABLE | SDHCI_CTRL_64BIT_ADDRESS;
    if (need_dll)
    {
        if (strict_dll)
            ret = emmc_dll_config_retry(1);
        else
        {
            ret = emmc_dll_config(1);
            if (ret != BIZ_SUCCESS)
            {
                HP_LOGI("[drv] emmc dll: warn pre-tuning stat=0x%x (continue)\n",
                           emmc_phy_readw(DLL_STATUS));
                ret = BIZ_SUCCESS;
            }
        }
        if (ret != BIZ_SUCCESS)
            return ret;
    }
    else
    {
        ret = emmc_dll_config(0);
        if (ret != BIZ_SUCCESS)
            return ret;
    }
    /* FPGA eMMC: AT_CTRL only in sdhci_exe_tuning(), not on every set_timing */
    emmc_writew(ctrl2, SDHCI_HOST_CONTROL2);
    return BIZ_SUCCESS;
}

static int emmc_set_bus_width_host(uint8_t width)
{
    uint8_t ctrl = emmc_readb(SDHCI_HOST_CONTROL);

    if (width == 8)
        ctrl |= 0x20;
    else if (width == 4)
    {
        ctrl |= 0x02;
        ctrl &= (uint8_t)~0x20;
    }
    else
    {
        ctrl &= (uint8_t)~(0x02 | 0x20);
    }

    emmc_writeb(ctrl, SDHCI_HOST_CONTROL);
    s_host.bus_width = width;
    return BIZ_SUCCESS;
}

static int emmc_set_power_180(void)
{
    emmc_writeb(SDHCI_POWER_ON | SDHCI_POWER_180, SDHCI_POWER_CONTROL);
    return BIZ_SUCCESS;
}

/* hp640 sdhci_init() / mmc_reinit() */
static int emmc_reinit(void)
{
    uint16_t ctrl2;
    int ret;

    /* sdhci_init: read vendor area before reset */
    emmc_read_vendor_offset();
    emmc_cache_host_version();

    emmc_reset(SDHCI_RESET_ALL);

    ctrl2 = emmc_readw(SDHCI_HOST_CONTROL2);
    ctrl2 |= SDHCI_CTRL_HOST_VER4_ENABLE | SDHCI_CTRL_64BIT_ADDRESS;
    ctrl2 &= (uint16_t)~SDHCI_CTRL_SPEED_MASK;
    ctrl2 |= SDHCI_CTRL_EMMC_Legacy;
    ctrl2 |= SDHCI_CTRL_CMD23_ENABLE;
    ctrl2 |= SDHCI_CTRL_VDD_180;
    emmc_writew(ctrl2, SDHCI_HOST_CONTROL2);

    /* sdhci_init: if (host->version > SDHCI_SPEC_300) host->v4_enable = true */
    s_v4_enable = (s_host_version > SDHCI_SPEC_300) ? 1U : 0U;

    emmc_set_power_180();
    emmc_writel(SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK, SDHCI_INT_ENABLE);
    emmc_writel(0, SDHCI_SIGNAL_ENABLE);

    ret = emmc_phy_init();
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_snps_set_ios_post();
    return BIZ_SUCCESS;
}

/* hp640 mmc_go_idle() — uses mmc_send_cmd, no init_in_progress gate */
static int emmc_go_idle(void)
{
    int ret;

    rt_hw_us_delay(1000);
    ret = emmc_send_cmd_raw(MMC_CMD_GO_IDLE_STATE, 0, 0);
    if (ret != BIZ_SUCCESS)
        return ret;
    rt_hw_us_delay(2000);
    return BIZ_SUCCESS;
}

/* hp640 spl_cmd.c init() */
static int emmc_init(void)
{
    int ret;

    ret = emmc_power_cycle();
    if (ret != BIZ_SUCCESS)
    {
        ret = emmc_power_on();
        if (ret != BIZ_SUCCESS)
            return ret;
    }

    ret = emmc_reinit();
    if (ret != BIZ_SUCCESS)
    {
        emmc_dump_regs("reinit");
        return ret;
    }

    s_host.init_in_progress = 0;
    s_host.legacy_speed = EMMC_LEGACY_SPEED_HZ;

    emmc_select_mode(EMMC_MODE_LEGACY);
    emmc_set_bus_width_host(1);
    ret = emmc_set_card_clock(0);
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = emmc_set_card_clock(EMMC_LEGACY_SPEED_HZ);
    if (ret != BIZ_SUCCESS)
    {
        emmc_dump_regs("legacyclk");
        return ret;
    }

    ret = emmc_go_idle();
    if (ret != BIZ_SUCCESS)
    {
        emmc_dump_regs("go_idle");
        return ret;
    }

    if (!ret)
        s_host.init_in_progress = 1;

    return ret;
}

/* hp640 try_init_emmc(): HS400 host setup before init_cmds() */
static int emmc_try_init_hs400_host(void)
{
    uint16_t reg;
    int ret;

    emmc_select_mode(EMMC_MODE_HS400);
    ret = emmc_set_clk_hz(s_host.tran_speed, 0);
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_snps_set_ios_post();

    reg = emmc_readw(SDHCI_HOST_CONTROL2);
    reg |= SDHCI_CTRL_UHS_MASK;
    emmc_writew(reg, SDHCI_HOST_CONTROL2);

    return emmc_sdhci_set_timing(EMMC_MODE_HS400, 0);
}

static int emmc_mmc_switch(uint8_t index, uint8_t value)
{
    uint32_t arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
                   ((uint32_t)index << 16) |
                   ((uint32_t)value << 8);
    int ret;

    ret = emmc_send_cmd(MMC_CMD_SWITCH, arg, 3);
    if (ret != BIZ_SUCCESS)
        return ret;

    rt_thread_mdelay(10);
    return BIZ_SUCCESS;
}

static int emmc_set_card_speed_mode(uint8_t mode, int hsdowngrade)
{
    uint8_t speed_bits;
    int ret;

    switch (mode)
    {
    case EMMC_MODE_HS:
        speed_bits = EXT_CSD_TIMING_HS;
        break;
    case EMMC_MODE_HS200:
        speed_bits = EXT_CSD_TIMING_HS200;
        break;
    case EMMC_MODE_HS400:
        speed_bits = EXT_CSD_TIMING_HS400;
        break;
    default:
        speed_bits = 0;
        break;
    }

    ret = emmc_mmc_switch(EXT_CSD_HS_TIMING, speed_bits);
    if (ret != BIZ_SUCCESS)
        return ret;

    if (hsdowngrade)
    {
        emmc_select_mode(EMMC_MODE_HS);
        emmc_sdhci_set_timing(EMMC_MODE_HS, 0);
        emmc_set_clk_hz(s_host.tran_speed, 0);
    }

    return BIZ_SUCCESS;
}

static int emmc_send_tuning_cmd(void)
{
    uint16_t blk_size = (s_host.bus_width == 8) ? 128U : 64U;
    uint16_t flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_DATA |
                     SDHCI_CMD_CRC | SDHCI_CMD_INDEX;
    uint32_t stat;
    uint32_t mask = SDHCI_INT_DATA_AVAIL;
    uint32_t inhibit_mask = SDHCI_CMD_INHIBIT;
    unsigned int time = 0;
    static unsigned int cmd_timeout = 100U;
    uint64_t poll_start;
    int ret = BIZ_SUCCESS;

    /* hp640 sdhci_send_command(): tuning skips DATA_INHIBIT */
    while (emmc_readl(SDHCI_PRESENT_STATE) & inhibit_mask)
    {
        if (time >= cmd_timeout)
        {
            if (cmd_timeout <= 400U)
                cmd_timeout += cmd_timeout;
            else
                return BIZ_ERR_EMMC_STATE_TIMEOUT;
        }
        time++;
        rt_hw_us_delay(1000);
    }

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);

    {
        uint8_t ctrl = emmc_readb(SDHCI_HOST_CONTROL);
        ctrl &= (uint8_t)~SDHCI_CTRL_DMA_MASK;
        emmc_writeb(ctrl, SDHCI_HOST_CONTROL);
    }

    emmc_writew(SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY, blk_size), SDHCI_BLOCK_SIZE);
    emmc_writew(1, SDHCI_BLOCK_COUNT);
    emmc_writew(SDHCI_TRNS_READ, SDHCI_TRANSFER_MODE);

    emmc_writel(0, SDHCI_ARGUMENT);
    emmc_writew(SDHCI_MAKE_CMD(MMC_CMD_SEND_TUNING_BLOCK_HS200, flags), SDHCI_COMMAND);
    emmc_dsb();

    /* hp640 sdhci_send_command(): no TIMEOUT_CONTROL for tuning (!data) */
    poll_start = emmc_cntpct();
    do {
        stat = emmc_readl(SDHCI_INT_STATUS);
        if (stat & SDHCI_INT_ERROR)
            break;
        if (emmc_ms_elapsed(poll_start) >= SDHCI_READ_STATUS_TIMEOUT)
            break;
    } while ((stat & mask) != mask);

    if ((stat & (SDHCI_INT_ERROR | mask)) == mask)
    {
        emmc_read_response(1);
        emmc_writel(mask, SDHCI_INT_STATUS);
    }
    else
        ret = BIZ_ERR_EMMC_INIT;

    /* hp640: SDHCI_QUIRK_WAIT_SEND_CMD before final INT clear (success or fail) */
    rt_hw_us_delay(1000);

    (void)emmc_readl(SDHCI_INT_STATUS);
    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);

    if (ret != BIZ_SUCCESS)
    {
        emmc_reset(SDHCI_RESET_CMD);
        emmc_reset(SDHCI_RESET_DATA);
    }

    (void)ret;
    return BIZ_SUCCESS;
}

static int emmc_tuning_latch(int iter, uint16_t ctrl, uint32_t tap)
{
    if (!(ctrl & SDHCI_CTRL_EXEC_TUNING))
        return 0;
    if ((iter >= 65 && tap >= 0x35U) ||
        (iter >= EMMC_TUNING_LATCH_MIN_ITER && tap >= EMMC_TUNING_TAP_MIN))
    {
        ctrl |= SDHCI_CTRL_TUNED_CLK;
        ctrl &= (uint16_t)~SDHCI_CTRL_EXEC_TUNING;
        emmc_writew(ctrl, SDHCI_HOST_CONTROL2);
        emmc_dsb();
        EMMC_BOOT_LOG("tuning latch tap=0x%x iter=%d\n", tap, iter);
        return 1;
    }
    return 0;
}

static int emmc_exe_tuning(void)
{
    uint16_t ctrl;
    int i, ret = BIZ_ERR_EMMC_TUNING_FAILED;

    ctrl = emmc_readw(SDHCI_HOST_CONTROL2);
    ctrl |= SDHCI_CTRL_EXEC_TUNING;
    ctrl &= (uint16_t)~SDHCI_CTRL_TUNED_CLK;
    emmc_writew(ctrl, SDHCI_HOST_CONTROL2);

    rt_hw_us_delay(1000);
    emmc_at_ctrl_config();

    emmc_writel(SDHCI_INT_DATA_AVAIL, SDHCI_INT_ENABLE);
    emmc_writel(SDHCI_INT_DATA_AVAIL, SDHCI_SIGNAL_ENABLE);

    for (i = 0; i < SDHCI_TUNING_LOOP_COUNT; i++)
    {
        (void)emmc_send_tuning_cmd();

        ctrl = emmc_readw(SDHCI_HOST_CONTROL2);

        if (emmc_tuning_latch(i, ctrl, emmc_at_stat_read()))
        {
            ret = BIZ_SUCCESS;
            break;
        }

        if (!(ctrl & SDHCI_CTRL_EXEC_TUNING))
        {
            if (ctrl & SDHCI_CTRL_TUNED_CLK)
            {
                uint32_t tap = emmc_at_stat_read();

                if (tap >= EMMC_TUNING_TAP_MIN)
                {
                    ret = BIZ_SUCCESS;
                    break;
                }
                EMMC_BOOT_LOG("tuning early tap=0x%x iter=%d, re-arm\n", tap, i);
                emmc_reset(SDHCI_RESET_CMD);
                emmc_reset(SDHCI_RESET_DATA);
                ctrl = emmc_readw(SDHCI_HOST_CONTROL2);
                ctrl &= (uint16_t)~SDHCI_CTRL_TUNED_CLK;
                ctrl |= SDHCI_CTRL_EXEC_TUNING;
                emmc_writew(ctrl, SDHCI_HOST_CONTROL2);
                rt_hw_us_delay(1000);
                emmc_at_ctrl_config();
                continue;
            }
            break;
        }

        EMMC_DBG_LOG("tuning iter=%d tap=0x%x HC2=0x%x\n",
                     i, emmc_at_stat_read(), ctrl);
    }

    emmc_writel(SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK, SDHCI_INT_ENABLE);
    emmc_writel(0, SDHCI_SIGNAL_ENABLE);

    if (ret != BIZ_SUCCESS)
    {
        ctrl = emmc_readw(SDHCI_HOST_CONTROL2);
        ctrl &= (uint16_t)~(SDHCI_CTRL_TUNED_CLK | SDHCI_CTRL_EXEC_TUNING);
        emmc_writew(ctrl, SDHCI_HOST_CONTROL2);
    }
    else
    {
        EMMC_BOOT_LOG("tuning OK tap=0x%x iter=%d\n", emmc_at_stat_read(), i);
        emmc_log_phy_at("tuning_ok");
    }

    return ret;
}

/* hp640 mmc_select_hs400() */
static int emmc_select_hs400(void)
{
    int ret;

    ret = emmc_set_card_speed_mode(EMMC_MODE_HS200, 0);
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_select_mode(EMMC_MODE_HS200);
    emmc_set_clk_hz(s_host.tran_speed, 0);
    ret = emmc_sdhci_set_timing(EMMC_MODE_HS200, 0);
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = emmc_exe_tuning();
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_select_mode(EMMC_MODE_HS);
    emmc_set_clk_hz(s_host.legacy_speed, 0);
    emmc_sdhci_set_timing(EMMC_MODE_HS, 0);
    ret = emmc_set_card_speed_mode(EMMC_MODE_HS, 0);
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = emmc_mmc_switch(EXT_CSD_BUS_WIDTH,
                          EXT_CSD_BUS_WIDTH_8 | EXT_CSD_DDR_FLAG);
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = emmc_set_card_speed_mode(EMMC_MODE_HS400, 0);
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_select_mode(EMMC_MODE_HS400);
    ret = emmc_set_clk_hz(s_host.tran_speed, 0);
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = emmc_sdhci_set_timing(EMMC_MODE_HS400, 0);
    if (ret == BIZ_SUCCESS)
        emmc_log_phy_at("hs400");

    return ret;
}

/* hp640 set_emmc_mode() with HS400_FPGA_MODE + MMC_HS_400 */
static int emmc_set_emmc_mode(void)
{
    uint8_t ext_csd_bits = EXT_CSD_BUS_WIDTH_8 | EXT_CSD_DDR_FLAG;
    int ret;

    ret = emmc_set_power_180();
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = emmc_mmc_switch(EXT_CSD_BUS_WIDTH, ext_csd_bits & ~EXT_CSD_DDR_FLAG);
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = emmc_set_bus_width_host(8);
    if (ret != BIZ_SUCCESS)
        return ret;

    return emmc_select_hs400();
}

static int emmc_send_cmd_raw(uint16_t cmdidx, uint32_t cmdarg, uint8_t resp_type)
{
    uint32_t stat;
    uint16_t flags = 0;
    unsigned int elapsed_us = 0;
    int ret = BIZ_SUCCESS;

    ret = emmc_wait_inhibit_us(100);
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);

    switch (resp_type)
    {
    case 0:
        flags = SDHCI_CMD_RESP_NONE;
        break;
    case 1:
        flags = SDHCI_CMD_RESP_SHORT;
        break;
    case 2:
        flags = SDHCI_CMD_RESP_LONG;
        break;
    case 3:
        flags = SDHCI_CMD_RESP_SHORT_BUSY;
        break;
    case 4:
        flags = SDHCI_CMD_RESP_SHORT; /* CMD1 R3 — no CRC */
        break;
    default:
        flags = SDHCI_CMD_RESP_SHORT;
        break;
    }

    if (resp_type != 0 && resp_type != 4)
        flags |= SDHCI_CMD_CRC | SDHCI_CMD_INDEX;

    if (resp_type == 3)
        emmc_writeb(SDHCI_DATA_TIMEOUT, SDHCI_TIMEOUT_CONTROL);

    {
        uint8_t ctrl = emmc_readb(SDHCI_HOST_CONTROL);
        ctrl &= ~SDHCI_CTRL_DMA_MASK;
        emmc_writeb(ctrl, SDHCI_HOST_CONTROL);
    }

    emmc_writel(cmdarg, SDHCI_ARGUMENT);
    emmc_writew(SDHCI_MAKE_CMD(cmdidx, flags), SDHCI_COMMAND);

    rt_hw_us_delay(1000); /* SDHCI_QUIRK_WAIT_SEND_CMD before poll */

    do {
        stat = emmc_readl(SDHCI_INT_STATUS);
        if (stat & SDHCI_INT_ERROR)
        {
            uint32_t fatal = stat & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC |
                                     SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                                     SDHCI_INT_ADMA_ERROR);

            if (fatal)
            {
                HP_LOGI("[drv] emmc CMD%d err stat=0x%x\n", cmdidx, stat);
                ret = emmc_map_cmd_error(stat);
                break;
            }
            /* hp640 __send_cmd(): INDEX/END_BIT alone with RESPONSE is not fatal */
            if (stat & SDHCI_INT_RESPONSE)
                break;
            HP_LOGI("[drv] emmc CMD%d err stat=0x%x\n", cmdidx, stat);
            ret = emmc_map_cmd_error(stat);
            break;
        }
        if (stat & SDHCI_INT_RESPONSE)
            break;
        if (elapsed_us >= SDHCI_READ_STATUS_TIMEOUT * 1000U)
        {
            if (resp_type == 3)
                ret = BIZ_SUCCESS; /* SDHCI_QUIRK_BROKEN_R1B */
            else
                ret = BIZ_ERR_EMMC_CMD_TIMEOUT;
            break;
        }
        rt_hw_us_delay(100);
        elapsed_us += 100;
    } while (1);

    if (ret == BIZ_SUCCESS)
    {
        emmc_read_response(resp_type);
        rt_hw_us_delay(1000); /* SDHCI_QUIRK_WAIT_SEND_CMD */
    }

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
    return ret;
}

static int emmc_send_cmd(uint16_t cmdidx, uint32_t cmdarg, uint8_t resp_type)
{
    if (!s_host.init_in_progress)
        return BIZ_ERR_EMMC_INIT;

    return emmc_send_cmd_raw(cmdidx, cmdarg, resp_type);
}

static int emmc_init_cmds(void)
{
    static const struct {
        uint16_t cmd;
        uint32_t arg;
    } seq[] = {
        {1, 0x00000000U},
        {1, 0x40360080U},
        {1, 0x40360080U},
        {1, 0x40360080U},
        {1, 0x40360080U},
        {2, 0x00000000U},
        {3, 0x00010000U},
        {9, 0x00010000U},
        {7, 0x00010000U},
    };
    int i, ret;

    for (i = 0; i < (int)(sizeof(seq) / sizeof(seq[0])); i++)
    {
        if (seq[i].cmd == 2)
            rt_thread_mdelay(10);

        ret = emmc_send_cmd_idx_retry(seq[i].cmd, seq[i].arg, 3);
        if (ret != BIZ_SUCCESS)
        {
            HP_LOGI("[drv] emmc init_cmds CMD%d fail (%d)\n", seq[i].cmd, ret);
            BIZ_ERROR("eMMC init_cmds CMD%d fail (%d)\n", seq[i].cmd, ret);
            return ret;
        }
    }
    return BIZ_SUCCESS;
}

static int build_adma_table(adma_desc_pair_t *pair, integrated_desc_t *interg,
                            int is_last, int is_read, uint32_t emmc_addr,
                            uint8_t *buf, uint32_t blk_size, uint32_t blk_cnt)
{
    uint32_t trans_bytes = blk_size * blk_cnt;
    uint32_t desc_count = (trans_bytes + ADMA_MAX_LEN - 1U) / ADMA_MAX_LEN;
    uint32_t i;
    uint16_t cmd_index;
    uint32_t mode;
    uint32_t flags;
    uint8_t attr;

    if (desc_count > EMMC_ADMA_DESC_LEN)
        return BIZ_ERR_EMMC_DATA_SIZE;

    pair->cmd_desc[0].attr = CMD_DESC_LINK_VALID;
    pair->cmd_desc[0].content = blk_cnt;
    pair->cmd_desc[1].attr = CMD_DESC_LINK_VALID;
    pair->cmd_desc[1].content = SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY, blk_size);
    pair->cmd_desc[2].attr = CMD_DESC_LINK_VALID;
    pair->cmd_desc[2].content = emmc_addr;

    mode = SDHCI_TRNS_DMA | SDHCI_TRNS_BLK_CNT_EN;
    if (is_read)
        mode |= SDHCI_TRNS_READ;
    if (blk_cnt > 1)
    {
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_ACMD23;
        cmd_index = is_read ? MMC_CMD_READ_MULTIPLE_BLOCK : MMC_CMD_WRITE_MULTIPLE_BLOCK;
    }
    else
    {
        cmd_index = is_read ? MMC_CMD_READ_SINGLE_BLOCK : MMC_CMD_WRITE_SINGLE_BLOCK;
    }

    flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_DATA | SDHCI_CMD_CRC | SDHCI_CMD_INDEX;
    pair->cmd_desc[3].attr = CMD_DESC_LINK_VALID;
    pair->cmd_desc[3].content = ((uint32_t)SDHCI_MAKE_CMD(cmd_index, flags) << 16) | mode;

    emmc_cache_flush(buf, trans_bytes);

    attr = ADMA_DESC_ATTR_VALID | ADMA_DESC_TRANSFER_DATA;
    for (i = 0; i + 1U < desc_count; i++)
    {
        pair->adma_desc[i].attr = attr;
        pair->adma_desc[i].len = ADMA_MAX_LEN;
        pair->adma_desc[i].addr_lo = (uint32_t)(uintptr_t)buf;
        pair->adma_desc[i].addr_hi = (uint32_t)((uintptr_t)buf >> 32);
        pair->adma_desc[i].rsvd = 0;
        buf += ADMA_MAX_LEN;
        trans_bytes -= ADMA_MAX_LEN;
    }

    pair->adma_desc[i].attr = attr | ADMA_DESC_ATTR_END;
    pair->adma_desc[i].len = (uint16_t)trans_bytes;
    pair->adma_desc[i].addr_lo = (uint32_t)(uintptr_t)buf;
    pair->adma_desc[i].addr_hi = (uint32_t)((uintptr_t)buf >> 32);
    pair->adma_desc[i].rsvd = 0;

    interg->attr = is_last ? INTERG_DESC_END_VALID : INTERG_DESC_LINK_VALID;
    interg->pointer_addr_low = (uint32_t)(uintptr_t)pair;
    interg->pointer_addr_hi = (uint32_t)((uintptr_t)pair >> 32);
    interg->rsvd2 = 0;

    emmc_cache_flush(pair, sizeof(*pair));
    emmc_cache_flush(interg, sizeof(*interg));
    emmc_cache_flush(pair->adma_desc, desc_count * sizeof(adma_desc_t));
    return BIZ_SUCCESS;
}

static int emmc_set_blocklen(uint16_t blk_size)
{
    int ret;

    /* hp640 mmc_set_blocklen(): skip CMD16 in DDR (HS400) mode */
    if (s_host.ddr_mode)
    {
        s_host.read_bl_len = blk_size;
        return BIZ_SUCCESS;
    }

    if (s_host.read_bl_len == blk_size)
        return BIZ_SUCCESS;

    ret = emmc_send_cmd(MMC_CMD_SET_BLOCKLEN, blk_size, 1);
    if (ret != BIZ_SUCCESS)
        return ret;

    s_host.read_bl_len = blk_size;
    return BIZ_SUCCESS;
}

/* hp640 auto_run(): set BLKSIZE + select ADMA3 after try_init_emmc() */
static void emmc_setup_adma3_host(void)
{
    uint8_t ctrl;

    if (s_host.read_bl_len != EMMC_BLK_SIZE)
    {
        s_host.read_bl_len = EMMC_BLK_SIZE;
        (void)emmc_set_blocklen(EMMC_BLK_SIZE);
    }

    ctrl = emmc_readb(SDHCI_HOST_CONTROL);
    ctrl &= (uint8_t)~SDHCI_CTRL_DMA_MASK;
    ctrl |= SDHCI_CTRL_SEL_ADMA3;
    emmc_writeb(ctrl, SDHCI_HOST_CONTROL);
}

int emmc_init_driver(void)
{
    int ret;

    emmc_chip_type_get();
    EMMC_BOOT_LOG("chip=%s HS400 SDCLK_DC=0x%x\n",
                  (s_emmc_chip_type == DRV_CHIP_KA200M) ? "KA200M" : "KA200",
                  emmc_hs400_sdclk_dc());

    ret = emmc_init();
    if (ret != BIZ_SUCCESS)
        goto fail;

    ret = emmc_try_init_hs400_host();
    if (ret != BIZ_SUCCESS)
        goto fail;

    ret = emmc_init_cmds();
    if (ret != BIZ_SUCCESS)
        goto fail;

    ret = emmc_set_emmc_mode();
    if (ret != BIZ_SUCCESS)
        goto fail;

    s_emmc_initialized = 1;
    emmc_setup_adma3_host();

    ret = emmc_wait_bus_idle(SDHCI_CMD_DEFAULT_TIMEOUT);
    if (ret != BIZ_SUCCESS)
        goto fail;

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
    emmc_writeb(SDHCI_DATA_TIMEOUT, SDHCI_TIMEOUT_CONTROL);

    EMMC_BOOT_LOG("HS400 OK tap=0x%x\n", emmc_at_stat_read());
    BIZ_INFO("eMMC HS400 init OK @ 0x%08x\n", (unsigned)EMMC_BASE);
    return BIZ_SUCCESS;

fail:
    drv_emmc_report_error((biz_err_code_t)ret);
    return ret;
}

static int emmc_xfer_data(uint32_t addr, uint8_t *buf, uint16_t blk_cnt,
                          uint16_t blk_size, int is_read)
{
    int ret = BIZ_SUCCESS;
    uint32_t time = 0;
    uint32_t cmd_timeout = SDHCI_CMD_DEFAULT_TIMEOUT;
    uint32_t mask = SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT;
    uint32_t trans_bytes = (uint32_t)blk_size * blk_cnt;
    uint32_t i = 0;
    uint8_t ctrl;

    if (!s_emmc_initialized)
        return BIZ_ERR_EMMC_INIT;
    if (!buf || blk_cnt == 0)
        return BIZ_ERR_NORMAL;
    if (trans_bytes > EMMC_MAX_TRANSFER)
        return BIZ_ERR_EMMC_DATA_SIZE;

    if (is_read)
    {
        s_emmc_stats.read_count++;
        s_emmc_stats.read_bytes += trans_bytes;
    }
    else
    {
        s_emmc_stats.write_count++;
        s_emmc_stats.write_bytes += trans_bytes;
    }

    /* hp640 emmc_write_data(): blocklen (skip CMD16 in DDR mode) */
    if (s_host.read_bl_len != blk_size)
    {
        s_host.read_bl_len = blk_size;
        ret = emmc_set_blocklen(blk_size);
        if (ret != BIZ_SUCCESS)
            goto out;
    }

    /* hp640 emmc_wait_bus_idle(): spin with extending timeout */
    while (emmc_readl(SDHCI_PRESENT_STATE) & mask)
    {
        if (time >= cmd_timeout * 100U)
        {
            if (cmd_timeout <= SDHCI_CMD_DEFAULT_TIMEOUT * 10U)
            {
                cmd_timeout += cmd_timeout;
            }
            else
            {
                ret = BIZ_ERR_EMMC_STATE_TIMEOUT;
                goto out;
            }
        }
        time++;
    }

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
    emmc_writeb(SDHCI_DATA_TIMEOUT, SDHCI_TIMEOUT_CONTROL);

    /* hp640 emmc_write_data(): select ADMA3 only (no emmc_setup_adma3_host) */
    ctrl = emmc_readb(SDHCI_HOST_CONTROL);
    ctrl &= (uint8_t)~SDHCI_CTRL_DMA_MASK;
    ctrl |= SDHCI_CTRL_SEL_ADMA2_ADMA3;
    emmc_writeb(ctrl, SDHCI_HOST_CONTROL);

    {
        integrated_desc_t *interg = emmc_dma_interg();
        adma_desc_pair_t *pair = emmc_dma_pair();

        rt_memset(interg, 0, sizeof(integrated_desc_t) * EMMC_MAX_ADMA_LINK);
        rt_memset(pair, 0, sizeof(adma_desc_pair_t) * EMMC_MAX_ADMA_LINK);

        i = 0;
        while (trans_bytes > EMMC_ADMA_DESC_LEN * ADMA_MAX_LEN)
        {
            uint32_t chunk_blks = EMMC_ADMA_DESC_LEN * ADMA_MAX_LEN / blk_size;
            uint32_t chunk_addr = addr + i * chunk_blks * blk_size;
            uint8_t *chunk_buf = buf + i * chunk_blks * blk_size;

            ret = build_adma_table(&pair[i], &interg[i], 0, is_read,
                                   chunk_addr, chunk_buf, blk_size, chunk_blks);
            if (ret != BIZ_SUCCESS)
                goto out;
            trans_bytes -= blk_size * chunk_blks;
            i++;
        }

        ret = build_adma_table(&pair[i], &interg[i], 1, is_read,
                               addr + i * (EMMC_ADMA_DESC_LEN * ADMA_MAX_LEN / blk_size) * blk_size,
                               buf + i * (EMMC_ADMA_DESC_LEN * ADMA_MAX_LEN / blk_size) * blk_size,
                               blk_size, trans_bytes / blk_size);
        if (ret != BIZ_SUCCESS)
            goto out;

        emmc_cache_flush(interg, sizeof(integrated_desc_t) * EMMC_MAX_ADMA_LINK);
        emmc_cache_flush(pair, sizeof(adma_desc_pair_t) * EMMC_MAX_ADMA_LINK);
    }
    emmc_cache_flush(buf, (rt_size_t)blk_cnt * blk_size);
    emmc_dma_flush_arena();
    emmc_dsb();

    emmc_prepare_hs400_xfer();

    /* hp640 emmc_write_data(): kick ADMA_ID then dump xfer_pre */
    emmc_kick_adma3(emmc_dma_interg());
    emmc_dsb();
    EMMC_REG_DUMP("xfer_pre");

    ret = emmc_wait_interrupt();

    if (ret != BIZ_SUCCESS)
    {
        if (is_read)
            s_emmc_stats.read_error_count++;
        else
            s_emmc_stats.write_error_count++;
        emmc_dump_regs("xfer_err");
    }
    else
        EMMC_REG_DUMP("xfer_ok");

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
    emmc_reset(SDHCI_RESET_CMD);
    emmc_reset(SDHCI_RESET_DATA);

    if (ret == BIZ_SUCCESS && is_read)
        emmc_cache_invalidate(buf, (rt_size_t)blk_cnt * blk_size);

out:
    if (ret != BIZ_SUCCESS)
        drv_emmc_report_error((biz_err_code_t)ret);
    return ret;
}

int emmc_read_data(uint32_t addr, uint8_t *buf, uint16_t blk_cnt, uint16_t blk_size)
{
    return emmc_xfer_data(addr, buf, blk_cnt, blk_size, 1);
}

int emmc_write_data(uint32_t addr, uint8_t *buf, uint16_t blk_cnt, uint16_t blk_size)
{
    return emmc_xfer_data(addr, buf, blk_cnt, blk_size, 0);
}

int drv_emmc_read_blocks(uint32_t addr, uint8_t *buf, uint16_t blk_cnt, uint16_t blk_size)
{
    return emmc_read_data(addr, buf, blk_cnt, blk_size);
}

int drv_emmc_write_blocks(uint32_t addr, uint8_t *buf, uint16_t blk_cnt, uint16_t blk_size)
{
    return emmc_write_data(addr, buf, blk_cnt, blk_size);
}

static int emmc_wait_interrupt(void)
{
    uint32_t stat;
    int ret = BIZ_SUCCESS;
    unsigned int spins = 0;

    do {
        stat = emmc_readl(SDHCI_INT_STATUS);
        if (stat & SDHCI_INT_ERROR)
        {
            if (stat & SDHCI_INT_TIMEOUT)
                ret = BIZ_ERR_EMMC_CMD_TIMEOUT;
            else if (stat & SDHCI_INT_CRC)
                ret = BIZ_ERR_EMMC_CMD_CRC;
            else if (stat & SDHCI_INT_DATA_TIMEOUT)
                ret = BIZ_ERR_EMMC_DATA_TIMEOUT;
            else if (stat & SDHCI_INT_DATA_CRC)
                ret = BIZ_ERR_EMMC_DATA_CRC;
            else
                ret = BIZ_ERR_EMMC_DATA_TIMEOUT;
            break;
        }

        if (stat & SDHCI_INT_DATA_END)
            break;

#ifdef BSP_DRV_EMMC_DEBUG
        if ((spins % 1000000U) == 0U && spins > 0U)
            EMMC_DBG_LOG("wait spin=%u INT=0x%x\n", spins, stat);
#endif

        if (++spins > 5000000U)
        {
            EMMC_BOOT_LOG("wait timeout INT=0x%x ADMA_ERR=0x%x\n",
                          stat, emmc_readb(SDHCI_ADMA_ERROR));
            ret = BIZ_ERR_EMMC_DATA_TIMEOUT;
            break;
        }
    } while (1);

    return ret;
}

int drv_emmc_exec_bd(uint64_t bd_addr)
{
    int ret;

    if (!s_emmc_initialized)
        return BIZ_ERR_EMMC_INIT;

    ret = emmc_wait_bus_idle(SDHCI_CMD_DEFAULT_TIMEOUT);
    if (ret != BIZ_SUCCESS)
        goto out;

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
    emmc_writeb(SDHCI_DATA_TIMEOUT, SDHCI_TIMEOUT_CONTROL);

    {
        uint8_t ctrl = emmc_readb(SDHCI_HOST_CONTROL);
        ctrl &= ~SDHCI_CTRL_DMA_MASK;
        ctrl |= SDHCI_CTRL_SEL_ADMA2_ADMA3;
        emmc_writeb(ctrl, SDHCI_HOST_CONTROL);
    }

    emmc_kick_adma3((const void *)(uintptr_t)bd_addr);

    ret = emmc_wait_interrupt();

    emmc_writel(SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
    emmc_reset(SDHCI_RESET_CMD);
    emmc_reset(SDHCI_RESET_DATA);

out:
    if (ret != BIZ_SUCCESS)
        drv_emmc_report_error((biz_err_code_t)ret);
    return ret;
}

int drv_emmc_try_init(bool force)
{
    int ret;

    if (!force && s_emmc_initialized)
        return BIZ_SUCCESS;

    ret = emmc_init_driver();
    if (ret != BIZ_SUCCESS)
    {
        biz_mcu_err_post((biz_err_code_t)ret);
        return ret;
    }

    return BIZ_SUCCESS;
}

int drv_emmc_available(void)
{
    return s_emmc_initialized ? 1 : 0;
}

int drv_emmc_init(void)
{
    /* hp640 snps_sdhci_probe(): snps_sdhci_clk_setup() before sdhci_probe */
    rt_memset(&s_emmc_stats, 0, sizeof(s_emmc_stats));
    emmc_soc_clk_enable();
    emmc_read_vendor_offset();
    emmc_cache_host_version();
    return BIZ_SUCCESS;
}

void drv_emmc_get_statistics(emmc_statistics_t *stats)
{
    if (stats)
        memcpy(stats, &s_emmc_stats, sizeof(*stats));
}
