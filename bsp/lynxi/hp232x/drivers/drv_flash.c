/*
 * drv_flash.c — KA200 SPI NOR (MX25L12805D) via DesignWare AHB SSI.
 * Ported from hp640_arm/common/spl/spl_cmd.c + lynxi AHB flash helpers.
 */

#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include "board.h"
#include "drv_flash.h"
#include "hp232x_mmu.h"
#include "biz_error_code.h"
#include "biz_log.h"
#include "tick.h"

/* SoC control */
#define FLASH_BOOT_SEL_REG          0x12500064UL
#define FLASH_XIP_CTRL_REG          0x12600024UL
#define FLASH_XIP_DISABLE_VAL         0xB8000000UL  /* hp640 open_flash */
#define FLASH_XIP_SPI_AHB_VAL         0x98000000UL  /* ofdata / lyn-sfc */
/* lynxi_cmd_spi AHB_STAND_CFG — Boot SSI pinmux after leave-XIP */
#define FLASH_IOC_SPI_MODE_REG      0x12000170UL
#define FLASH_IOC_SPI_MODE_AHB      0x208U
#define FLASH_IOC_SPI0_REG          0x12000118UL
#define FLASH_IOC_SPI1_REG          0x1200011CUL
#define FLASH_IOC_SPI2_REG          0x1200014CUL
#define FLASH_IOC_SPI3_REG          0x12000148UL
#define FLASH_IOC_SPI_VAL           0x660aU

/* Boot SSI register windows — lynxi-uboot DTS ssi2 / lyn-sfc sfc_nor1 */
#define FLASH_SSI_REG0              0x02000000UL  /* SYS_ROM  win=0 */
#define FLASH_SSI_REG1              0x00000000UL  /* BOOT_SSI win=1 (PA) */
#define FLASH_SSI_REG2              0x06000000UL  /* SYS_IRAM win=2 */
/*
 * Like Linux ioremap(PA0): CPU uses high VA mapped to PA0, not VA=0.
 * See hp232x_mmu.c L2 alias 0x07000000 → 0x00000000.
 */
#define FLASH_SSI_PA0_VA            0x07000000UL

#define FLASH_BUS_CLK_HZ            100000000UL
#define FLASH_SPEED_PROBE           100000UL
#define FLASH_SPEED_NORMAL          24000000UL
#define FLASH_DEFAULT_CS            0

/* DesignWare SSI registers */
#define DW_SPI_CTRL0                0x00
#define DW_SPI_CTRL1                0x04
#define DW_SPI_SSIENR               0x08
#define DW_SPI_SER                  0x10
#define DW_SPI_BAUDR                0x14
#define DW_SPI_TXFLTR               0x18
#define DW_SPI_RXFLTR               0x1c
#define DW_SPI_TXFLR                0x20
#define DW_SPI_RXFLR                0x24
#define DW_SPI_SR                   0x28
#define DW_SPI_DR                   0x60

#define SR_BUSY                     0x01
#define SR_TF_NOT_FULL              0x02
#define SR_TF_EMPT                  0x04
#define SR_RF_NOT_EMPT              0x08

#define SPI_AHB_FIFO_HALF           32

#define NOR_CMD_READ                0x03
#define NOR_CMD_READ_ID             0x9F
#define NOR_CMD_WREN                0x06
#define NOR_CMD_RDSR                0x05
#define NOR_CMD_PP                  0x02
#define NOR_CMD_SE                  0x20
#define NOR_CMD_BE64K               0xD8

#define NOR_JEDEC_MX25L12805D       0xC22018UL

static int flash_jedec_is_known(uint32_t jedec)
{
    static const uint32_t known[] = {
        NOR_JEDEC_MX25L12805D,  /* mx25l12805d — EVB DTS */
        0xC22618UL,               /* mx25l12855e */
        0xC22537UL,               /* mx25u6432 / mx25u6435 — board JEDEC */
        0x61129BUL,               /* ISSI IS25LP128 — 58.36 EVB */
    };
    rt_size_t i;

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    {
        if (jedec == known[i])
            return 1;
    }
    return 0;
}

#define SPI_TMOD_TR                 0
#define SPI_TMOD_TX                 1
#define SPI_TMOD_EEPROM             3

typedef struct
{
    volatile uint32_t *regs;
    uint32_t jedec_id;
    int ready;
} drv_flash_ctx_t;

static drv_flash_ctx_t s_flash;

#ifndef ARCH_DMA_MINALIGN
#define ARCH_DMA_MINALIGN 64
#endif

/* Invalidate covering full cache lines (no-op for Normal-NC host scratch). */
static void flash_dcache_inv(const void *p, int len)
{
    uintptr_t start;
    uintptr_t end;
    const uintptr_t line = (uintptr_t)ARCH_DMA_MINALIGN;

    if (!p || len <= 0 || hp232x_addr_is_normal_nc(p, (size_t)len))
        return;

    start = (uintptr_t)p & ~(line - 1U);
    end = ((uintptr_t)p + (uintptr_t)len + line - 1U) & ~(line - 1U);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)start, (int)(end - start));
}

/* Clean WB dirty lines to DRAM (I2C OTA CPU memcpy); no-op for NC. */
static void flash_src_dcache_clean(const void *src, int len)
{
    uintptr_t line = (uintptr_t)ARCH_DMA_MINALIGN;
    uintptr_t start;
    uintptr_t end;

    if (!src || len <= 0 || hp232x_addr_is_normal_nc(src, (size_t)len))
        return;

    start = (uintptr_t)src & ~(line - 1U);
    end = ((uintptr_t)src + (uintptr_t)len + line - 1U) & ~(line - 1U);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)start, (int)(end - start));
}

#if defined(RT_USING_SMP) && defined(BSP_FLASH_CPU0_WORKER) && \
    !defined(BSP_FLASH_DIRECT_ON_CALLER)
/*
 * Production: bounce Flash/SSI to CPU0 worker. emmc_biz stays on CPU1 at
 * highest biz priority; flash worker does not preempt the biz busy-loop.
 * (Direct CPU1 SSI is OK after EPROMREAD/TO fixes; bounce remains preferred.)
 */
enum {
    FLASH_JOB_READ = 1,
    FLASH_JOB_WRITE,
    FLASH_JOB_ERASE,
};

typedef struct
{
    int op;
    const void *src;
    void *dst;
    int len;
    uint32_t addr;
    int ret;
} flash_job_t;

static struct
{
    flash_job_t job;
    rt_sem_t req;
    rt_sem_t done;
    rt_mutex_t lock;
    rt_thread_t worker;
    int worker_ready;
} s_flash_svc;

static int flash_write_local(const void *src, int len, uint32_t addr);
static int flash_read_local(void *dst, int len, uint32_t addr);
static int flash_erase_local(uint32_t addr, int len);

static void flash_worker_entry(void *param)
{
    (void)param;

    while (1)
    {
        rt_sem_take(s_flash_svc.req, RT_WAITING_FOREVER);
        switch (s_flash_svc.job.op)
        {
        case FLASH_JOB_READ:
            s_flash_svc.job.ret = flash_read_local(s_flash_svc.job.dst,
                                                   s_flash_svc.job.len,
                                                   s_flash_svc.job.addr);
            break;
        case FLASH_JOB_WRITE:
            s_flash_svc.job.ret = flash_write_local(s_flash_svc.job.src,
                                                    s_flash_svc.job.len,
                                                    s_flash_svc.job.addr);
            break;
        case FLASH_JOB_ERASE:
            s_flash_svc.job.ret = flash_erase_local(s_flash_svc.job.addr,
                                                    s_flash_svc.job.len);
            break;
        default:
            s_flash_svc.job.ret = BIZ_ERR_NORMAL;
            break;
        }
        rt_sem_release(s_flash_svc.done);
    }
}

static int flash_svc_call(int op, const void *src, void *dst, int len, uint32_t addr)
{
    int ret;

    if (!s_flash_svc.worker_ready)
        return BIZ_ERR_NORMAL;

    /* Caller CPU: WB CPU stores must hit DRAM before bounce/inv (I2C OTA). */
    if (op == FLASH_JOB_WRITE && src && len > 0)
    {
        flash_src_dcache_clean(src, len);
        flash_dcache_inv(src, len);
    }

    rt_mutex_take(s_flash_svc.lock, RT_WAITING_FOREVER);
    s_flash_svc.job.op = op;
    s_flash_svc.job.src = src;
    s_flash_svc.job.dst = dst;
    s_flash_svc.job.len = len;
    s_flash_svc.job.addr = addr;
    s_flash_svc.job.ret = BIZ_ERR_NORMAL;
    rt_sem_release(s_flash_svc.req);
    rt_sem_take(s_flash_svc.done, RT_WAITING_FOREVER);
    ret = s_flash_svc.job.ret;
    rt_mutex_release(s_flash_svc.lock);
    return ret;
}

static int flash_svc_start(void)
{
    rt_thread_t t;

    if (s_flash_svc.worker_ready)
        return 0;

    s_flash_svc.req = rt_sem_create("fl_req", 0, RT_IPC_FLAG_FIFO);
    s_flash_svc.done = rt_sem_create("fl_done", 0, RT_IPC_FLAG_FIFO);
    s_flash_svc.lock = rt_mutex_create("fl_lk", RT_IPC_FLAG_PRIO);
    if (!s_flash_svc.req || !s_flash_svc.done || !s_flash_svc.lock)
        return -1;

    t = rt_thread_create("flash", flash_worker_entry, RT_NULL,
                         8192, 5, 20);
    if (!t)
        return -1;
    s_flash_svc.worker = t;
    rt_thread_startup(t);
    s_flash_svc.worker_ready = 1;
    return 0;
}
#endif /* RT_USING_SMP && BSP_FLASH_CPU0_WORKER && !DIRECT */

static inline uint32_t dw_read(volatile uint32_t *regs, uint32_t off)
{
    return *(regs + (off / 4));
}

static inline void dw_write(volatile uint32_t *regs, uint32_t off, uint32_t val)
{
    *(regs + (off / 4)) = val;
}

static void flash_xip_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

extern void __asm_flush_dcache_all(void);
extern void __asm_invalidate_dcache_all(void);
extern void __asm_invalidate_icache_all(void);

/* lynxi_cmd_spi AHB_STAND_CFG after 0x98 — Boot SSI pads. */
static void flash_ssi_ahb_pinmux(void)
{
    *(volatile uint32_t *)FLASH_IOC_SPI_MODE_REG = FLASH_IOC_SPI_MODE_AHB;
    *(volatile uint32_t *)FLASH_IOC_SPI0_REG = FLASH_IOC_SPI_VAL;
    *(volatile uint32_t *)FLASH_IOC_SPI1_REG = FLASH_IOC_SPI_VAL;
    *(volatile uint32_t *)FLASH_IOC_SPI2_REG = FLASH_IOC_SPI_VAL;
    *(volatile uint32_t *)FLASH_IOC_SPI3_REG = FLASH_IOC_SPI_VAL;
    flash_xip_barrier();
}

/*
 * Align hp640 open_flash: flush_dcache_all + invalidate_icache, then B8→98.
 * Pure invalidate_dcache_all drops dirty MMU/stack lines — Flash-cold secondary
 * then EXC in hp232x_mmu_secondary_init (UART often survives). Use clean+inv.
 */
static void flash_xip_leave_open_flash(void)
{
    int try;

    for (try = 0; try < 3; try++)
    {
        __asm_flush_dcache_all();
        __asm_invalidate_icache_all();
        *(volatile uint32_t *)FLASH_XIP_CTRL_REG = FLASH_XIP_DISABLE_VAL;
        flash_xip_barrier();
        /* open_flash→ofdata gap; B8 then immediately 98 was unreliable on cold. */
        rt_hw_us_delay(100);
        *(volatile uint32_t *)FLASH_XIP_CTRL_REG = FLASH_XIP_SPI_AHB_VAL;
        flash_xip_barrier();
        flash_ssi_ahb_pinmux();
        rt_hw_us_delay(50);
        if (*(volatile uint32_t *)FLASH_XIP_CTRL_REG == FLASH_XIP_SPI_AHB_VAL)
        {
            HP_LOGI("[drv] flash leave-XIP ok ssi_ctrl=0x%08x\n",
                       *(volatile uint32_t *)FLASH_XIP_CTRL_REG);
            return;
        }
    }
    HP_LOGI("[drv] flash leave-XIP warn: ssi_ctrl=0x%08x (want 0x%08x)\n",
               *(volatile uint32_t *)FLASH_XIP_CTRL_REG,
               (unsigned)FLASH_XIP_SPI_AHB_VAL);
}

/*
 * Soft-read PA0 alias: under XIP this returns ROM opcodes (no SEA);
 * after true leave-XIP, DW CTRL0 looks like 0x008f481f (not AArch64 code).
 * Never write SSI until this passes — write while XIP → SError FAR=0x120.
 */
static int flash_ssi_window_ready(volatile uint32_t *regs)
{
    uint32_t word;

    if (!regs)
        return 0;

    word = dw_read(regs, DW_SPI_CTRL0);
    /* XIP/image fetch: typical AArch64 sys/branch encodings around PA0. */
    if ((word & 0xff000000U) == 0xd5000000U ||
        (word & 0xff000000U) == 0xd1000000U ||
        (word & 0xfc000000U) == 0x14000000U ||
        (word & 0xff000000U) == 0xaa000000U)
    {
        HP_LOGI("[drv] flash SSI@%p still XIP-like CTRL0=0x%08x (no MMIO write)\n",
                   regs, word);
        return 0;
    }

    HP_LOGI("[drv] flash SSI window ready CTRL0=0x%08x @%p\n", word, regs);
    return 1;
}

/* Ongoing R/W: keep AHB SPI mode (ofdata 0x98000000). */
static void flash_xip_spi_mode(void)
{
    *(volatile uint32_t *)FLASH_XIP_CTRL_REG = FLASH_XIP_SPI_AHB_VAL;
    flash_xip_barrier();
}

static int flash_region_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1)
{
    return (a0 < b1) && (a1 > b0);
}

int drv_flash_bootcode_overlap(uint32_t addr, uint32_t len, int include_erase)
{
    uint32_t end;
    uint32_t op_start = addr;
    uint32_t op_end;

    if (len == 0U)
        return 0;

    end = addr + (uint32_t)len;
    if (end <= addr)
        return 1;

    if (include_erase)
    {
        op_start = (addr / DRV_FLASH_SUBSECTOR_SIZE) * DRV_FLASH_SUBSECTOR_SIZE;
        op_end = op_start +
                 (((uint32_t)len + DRV_FLASH_SUBSECTOR_SIZE - 1U) / DRV_FLASH_SUBSECTOR_SIZE) *
                 DRV_FLASH_SUBSECTOR_SIZE;
    }
    else
    {
        op_end = end;
    }

    return flash_region_overlap(op_start, op_end, 0U, DRV_FLASH_XIP_BOOTCODE_END);
}

/*
 * Strap window (HW SEL). win=1 → PA0 via VA alias (lyn-sfc ioremap).
 * Leave-XIP: hp640 open_flash (B8) + ofdata (98).
 * Call after scheduler is up. Soft-read gate before any SSI write (SEA when XIP).
 */
static volatile uint32_t *flash_regs_from_boot_sel(void)
{
    static const uint32_t windows[3] = {
        FLASH_SSI_REG0,
        FLASH_SSI_REG1,
        FLASH_SSI_REG2,
    };
    uint32_t boot_sel_full;
    uint32_t win;
    uint32_t base;
    uint64_t desc = 0;
    uint32_t attr = 0;
    int mmu_ret;
    volatile uint32_t *regs;

    flash_xip_leave_open_flash();

    boot_sel_full = *(volatile uint32_t *)FLASH_BOOT_SEL_REG;
    win = (boot_sel_full >> 28) & 0x3U;
    if (win > 2U)
        win = 0U;
    base = windows[win];

    if (base == 0U)
        base = FLASH_SSI_PA0_VA;

    mmu_ret = hp232x_mmu_check_l2_block((uint64_t)base,
                                        (windows[win] == 0U) ? 0UL : (uint64_t)base,
                                        &desc, &attr);
    HP_LOGI("[drv] flash BOOT_SELECT=0x%08x win=%u SSI@0x%08lx%s mmu_ret=%d desc=0x%llx attr=%u\n",
               boot_sel_full, win, (unsigned long)base,
               (windows[win] == 0U) ? " (PA0 via VA alias)" : "",
               mmu_ret, (unsigned long long)desc, attr);

    regs = (volatile uint32_t *)(uintptr_t)base;
    if (!flash_ssi_window_ready(regs))
        return RT_NULL;
    return regs;
}

static void flash_enable_chip(int enable)
{
    dw_write(s_flash.regs, DW_SPI_SSIENR, enable ? 1U : 0U);
}

static void flash_set_baud(uint32_t speed_hz)
{
    uint32_t div;

    if (speed_hz > FLASH_BUS_CLK_HZ / 2U)
        speed_hz = FLASH_BUS_CLK_HZ / 2U;

    div = FLASH_BUS_CLK_HZ / speed_hz;
    div = (div + 1U) & ~1U;
    if (div < 2U)
        div = 2U;

    dw_write(s_flash.regs, DW_SPI_BAUDR, div);
}

static uint32_t flash_build_ctr0(uint8_t tmod)
{
    uint32_t ctr0 = 0;

    /* 8-bit frames, SPI mode 0 (CPOL=0, CPHA=0), AHB master — match hp640 flash */
    ctr0 |= (7U << 0);
    ctr0 |= (0U << 6);
    ctr0 |= (0U << 8);   /* SCPH */
    ctr0 |= (0U << 9);   /* SCPOL */
    ctr0 |= ((uint32_t)tmod << 10);
    ctr0 |= (1U << 31);  /* SSI_IS_MST */
    return ctr0;
}

static void flash_rx_drain(void)
{
    int guard = 256;

    while ((dw_read(s_flash.regs, DW_SPI_SR) & SR_RF_NOT_EMPT) && guard-- > 0)
        (void)dw_read(s_flash.regs, DW_SPI_DR);
}

static void flash_ahb_cfg(uint32_t speed_hz, uint8_t tmod, uint32_t ndf)
{
    flash_enable_chip(0);
    flash_rx_drain();
    flash_set_baud(speed_hz);
    dw_write(s_flash.regs, DW_SPI_SER, 1U << FLASH_DEFAULT_CS);
    dw_write(s_flash.regs, DW_SPI_CTRL1, ndf);
    dw_write(s_flash.regs, DW_SPI_CTRL0, flash_build_ctr0(tmod));
    dw_write(s_flash.regs, DW_SPI_TXFLTR, 0x1f);
    /* Align lyn-sfc: RXFLTR=0 for poll/PIO EEPROM+TO paths */
    dw_write(s_flash.regs, DW_SPI_RXFLTR, 0);
    flash_enable_chip(1);
}

static int flash_wait_tx_idle(void)
{
    int timeout = 20000;

    while (((dw_read(s_flash.regs, DW_SPI_SR) & SR_TF_EMPT) == 0U) ||
           (dw_read(s_flash.regs, DW_SPI_SR) & SR_BUSY))
    {
        rt_hw_us_delay(2);
        if (--timeout == 0)
            return -1;
    }

    return 0;
}

/* Push TX FIFO only — for EEPROM-mode command phase (RX comes after). */
static int flash_tx_push(const uint8_t *tx, int num)
{
    int sent = 0;

    while (sent < num)
    {
        int chunk = num - sent;
        int timeout = 20000;

        if (chunk > SPI_AHB_FIFO_HALF)
            chunk = SPI_AHB_FIFO_HALF;

        while ((dw_read(s_flash.regs, DW_SPI_SR) & SR_TF_NOT_FULL) == 0U)
        {
            rt_hw_us_delay(2);
            if (--timeout == 0)
                return -1;
        }

        for (int i = 0; i < chunk; i++)
            dw_write(s_flash.regs, DW_SPI_DR, tx[sent + i]);

        sent += chunk;
    }

    return 0;
}

/*
 * Transmit-only, keep CS by never letting TXFIFO fully drain mid-frame.
 * Used with SPI_TMOD_TX (TO). Clear RX overflow if any leftover.
 */
static int flash_tx_only(const uint8_t *tx, int num)
{
    int sent = 0;
    int idle = 0;

    while (sent < num)
    {
        if (dw_read(s_flash.regs, DW_SPI_SR) & SR_TF_NOT_FULL)
        {
            dw_write(s_flash.regs, DW_SPI_DR, tx[sent++]);
            idle = 0;
            /* Clear sticky RX overflow (TO should not fill RX). */
            if ((sent & 0x1f) == 0)
                (void)dw_read(s_flash.regs, 0x3c); /* RXOICR */
            continue;
        }
        rt_hw_us_delay(1);
        if (++idle > 200000)
            return -1;
    }

    return flash_wait_tx_idle();
}

static int flash_eeprom_read(const uint8_t *hdr, int hdr_len, uint8_t *rx, int rx_len)
{
    int rx_done = 0;
    int timeout;

    /* EEPROM mode: push cmd/addr only; SSI then fills RX with NDF+1 bytes. */
    if (flash_tx_push(hdr, hdr_len) != 0)
        return -1;

    while (rx_done < rx_len)
    {
        int chunk = rx_len - rx_done;

        if (chunk > SPI_AHB_FIFO_HALF)
            chunk = SPI_AHB_FIFO_HALF;

        timeout = 20000;
        while (dw_read(s_flash.regs, DW_SPI_RXFLR) < (uint32_t)chunk)
        {
            rt_hw_us_delay(1);
            if (--timeout == 0)
                return -1;
        }

        for (int i = 0; i < chunk; i++)
            rx[rx_done + i] = (uint8_t)dw_read(s_flash.regs, DW_SPI_DR);

        rx_done += chunk;
    }

    return flash_wait_tx_idle();
}

static void flash_close(void)
{
    flash_enable_chip(0);
    dw_write(s_flash.regs, DW_SPI_SER, 0);
}

/*
 * Read NOR register(s) — same as lyn-sfc lyn_spi_nor_read_reg:
 * TMOD_EPROMREAD, TX=opcode only, RX=len bytes (no TR command dummy).
 */
static int flash_nor_read_reg(uint8_t opcode, uint8_t *buf, int len)
{
    uint8_t hdr = opcode;
    int ret;

    if (!buf || len <= 0)
        return -1;

    flash_ahb_cfg(FLASH_SPEED_NORMAL, SPI_TMOD_EEPROM, (uint32_t)len - 1U);
    ret = flash_eeprom_read(&hdr, 1, buf, len);
    flash_close();
    return ret;
}

static int flash_nor_wait_ready(void)
{
    uint8_t sr = 0xFF;
    int timeout = 1000000;

    while (timeout-- > 0)
    {
        if (flash_nor_read_reg(NOR_CMD_RDSR, &sr, 1) != 0)
            return -1;
        if ((sr & 0x01U) == 0U) /* WIP */
            return 0;
        rt_hw_us_delay(10);
    }

    return -1;
}

static int flash_nor_write_enable(void)
{
    uint8_t cmd = NOR_CMD_WREN;
    uint8_t sr = 0;

    flash_ahb_cfg(FLASH_SPEED_NORMAL, SPI_TMOD_TX, 0);
    if (flash_tx_only(&cmd, 1) != 0)
        return -1;
    flash_close();

    if (flash_nor_read_reg(NOR_CMD_RDSR, &sr, 1) != 0)
        return -1;
    if ((sr & 0x02U) == 0U) /* WEL */
        return -1;
    return 0;
}

static int flash_nor_read_data(uint32_t addr, void *dst, int len)
{
    uint8_t hdr[4];
    int ret;

    if (!dst || len <= 0)
        return -1;

    hdr[0] = NOR_CMD_READ;
    hdr[1] = (addr >> 16) & 0xFFU;
    hdr[2] = (addr >> 8) & 0xFFU;
    hdr[3] = addr & 0xFFU;

    flash_ahb_cfg(FLASH_SPEED_NORMAL, SPI_TMOD_EEPROM, (uint32_t)len - 1U);
    ret = flash_eeprom_read(hdr, 4, dst, len);
    flash_close();
    return ret;
}

static int flash_nor_erase_subsector(uint32_t addr)
{
    uint8_t cmd[4];
    int ret;

    ret = flash_nor_write_enable();
    if (ret != 0)
        return ret;

    cmd[0] = NOR_CMD_SE;
    cmd[1] = (addr >> 16) & 0xFFU;
    cmd[2] = (addr >> 8) & 0xFFU;
    cmd[3] = addr & 0xFFU;

    flash_ahb_cfg(FLASH_SPEED_NORMAL, SPI_TMOD_TX, 0);
    ret = flash_tx_only(cmd, 4);
    flash_close();
    if (ret != 0)
        return ret;

    return flash_nor_wait_ready();
}

static int flash_nor_erase_block(uint32_t addr)
{
    uint8_t cmd[4];
    int ret;

    ret = flash_nor_write_enable();
    if (ret != 0)
        return ret;

    cmd[0] = NOR_CMD_BE64K;
    cmd[1] = (addr >> 16) & 0xFFU;
    cmd[2] = (addr >> 8) & 0xFFU;
    cmd[3] = addr & 0xFFU;

    flash_ahb_cfg(FLASH_SPEED_NORMAL, SPI_TMOD_TX, 0);
    ret = flash_tx_only(cmd, 4);
    flash_close();
    if (ret != 0)
        return ret;

    return flash_nor_wait_ready();
}

static int flash_nor_page_program(uint32_t addr, const uint8_t *src, int len)
{
    uint8_t buf[4 + DRV_FLASH_PAGE_SIZE];
    int ret;

    if (len <= 0 || len > DRV_FLASH_PAGE_SIZE)
        return -1;

    ret = flash_nor_write_enable();
    if (ret != 0)
        return ret;

    /* One SPI frame: PP + addr + payload (CS must stay asserted). */
    buf[0] = NOR_CMD_PP;
    buf[1] = (addr >> 16) & 0xFFU;
    buf[2] = (addr >> 8) & 0xFFU;
    buf[3] = addr & 0xFFU;
    memcpy(buf + 4, src, (size_t)len);

    flash_ahb_cfg(FLASH_SPEED_NORMAL, SPI_TMOD_TX, 0);
    ret = flash_tx_only(buf, 4 + len);
    flash_close();
    if (ret != 0)
        return ret;

    return flash_nor_wait_ready();
}

static int flash_run_read(void *dst, int len, uint32_t addr)
{
    flash_xip_spi_mode();
    return flash_nor_read_data(addr, dst, len);
}

/*
 * Program from WB IRAM that eMMC loaded on another CPU.
 * Pull each page into a local buffer (after inv) so PP never streams
 * stale cachelines (symptom: verify wr=real rd=0xff). Then readback
 * that page and re-PP if still blank — avoids full-chip erase retries.
 */
static int flash_run_write(const void *src, int len, uint32_t addr)
{
    const uint8_t *p = src;
    int left = len;
    uint32_t cur = addr;
    uint8_t page[DRV_FLASH_PAGE_SIZE];
    uint8_t vfy[DRV_FLASH_PAGE_SIZE];
    int ret;

    flash_xip_spi_mode();

    while (left > 0)
    {
        int page_off = cur % DRV_FLASH_PAGE_SIZE;
        int chunk = DRV_FLASH_PAGE_SIZE - page_off;
        int try;

        if (chunk > left)
            chunk = left;

        flash_dcache_inv(p, chunk);
        memcpy(page, p, (size_t)chunk);

        for (try = 0; try < 4; try++)
        {
            ret = flash_nor_page_program(cur, page, chunk);
            if (ret != 0)
                return ret;

            if (flash_nor_read_data(cur, vfy, chunk) != 0)
                return -1;

            if (memcmp(page, vfy, (size_t)chunk) == 0)
                break;
        }
        if (try >= 4)
        {
            BIZ_ERROR("[flash] page PP verify fail addr=0x%x len=%d\n",
                      (unsigned)cur, chunk);
            return -1;
        }

        p += chunk;
        cur += (uint32_t)chunk;
        left -= chunk;
    }

    return 0;
}

static int flash_run_erase(uint32_t addr, int len)
{
    uint32_t end = addr + (uint32_t)len;
    uint32_t cur = addr & ~(DRV_FLASH_SUBSECTOR_SIZE - 1U);
    int ret;

    flash_xip_spi_mode();

    /* Prefer 64KB block erase when aligned (hp640 spi_flash_erase sector). */
    while (cur < end)
    {
        if (((cur & (DRV_FLASH_SECTOR_SIZE - 1U)) == 0U) &&
            ((end - cur) >= DRV_FLASH_SECTOR_SIZE))
        {
            ret = flash_nor_erase_block(cur);
            if (ret != 0)
                return ret;
            cur += DRV_FLASH_SECTOR_SIZE;
        }
        else
        {
            ret = flash_nor_erase_subsector(cur);
            if (ret != 0)
                return ret;
            cur += DRV_FLASH_SUBSECTOR_SIZE;
        }
    }

    return 0;
}

static int flash_probe_jedec(void)
{
    uint8_t id[3] = { 0 };
    int ret;

    flash_xip_spi_mode();

    /* lyn-sfc / hp640: EPROMREAD 0x9F → 3 ID bytes (no TR dummy) */
    flash_ahb_cfg(FLASH_SPEED_PROBE, SPI_TMOD_EEPROM, 2U);
    {
        uint8_t hdr = NOR_CMD_READ_ID;

        ret = flash_eeprom_read(&hdr, 1, id, 3);
        flash_close();
    }

    if (ret != 0)
        return BIZ_ERR_FLASH_INIT;

    s_flash.jedec_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];

    if (!flash_jedec_is_known(s_flash.jedec_id))
    {
        BIZ_WARN("Flash JEDEC 0x%06x (expect 0x%06x), raw eeprom=%02x %02x %02x\n",
                 s_flash.jedec_id, (unsigned)NOR_JEDEC_MX25L12805D,
                 id[0], id[1], id[2]);
    }

    flash_xip_spi_mode();
    s_flash.ready = 1;
    return BIZ_SUCCESS;
}

int drv_flash_init(void)
{
    uint32_t boot_sel;
    uint32_t win;

    if (s_flash.ready)
        return BIZ_SUCCESS;

    /*
     * Soft only at INIT_DEVICE. Cold jumper: early SSI/JEDEC → SError even when
     * MMU alias looks OK (mmu_ret=0). Full bring-up: drv_flash_bringup() in main
     * (or msh flash init when BSP_FLASH_DEFER_INIT).
     */
    boot_sel = *(volatile uint32_t *)FLASH_BOOT_SEL_REG;
    win = (boot_sel >> 28) & 0x3U;
    s_flash.regs = RT_NULL;
    s_flash.jedec_id = 0;
    HP_LOGI("[drv] flash soft-init (SSI deferred). BOOT_SELECT=0x%08x win=%u\n",
               boot_sel, win);
#ifdef BSP_FLASH_DEFER_INIT
    HP_LOGI("[drv] flash msh: status | open_flash | bind | peek | jedec | init\n");
#endif
    return BIZ_SUCCESS;
}

int drv_flash_bringup(void)
{
    if (s_flash.ready)
        return BIZ_SUCCESS;

    /* leave-XIP + soft window check; abort before SSI write if still XIP */
    s_flash.regs = flash_regs_from_boot_sel();
    if (!s_flash.regs)
    {
        HP_LOGI("[drv] flash bringup skip JEDEC (SSI window not ready)\n");
        return BIZ_ERR_FLASH_INIT;
    }

    if (flash_probe_jedec() != BIZ_SUCCESS)
        return BIZ_ERR_FLASH_INIT;
    HP_LOGI("[drv] flash JEDEC=0x%06x ready SSI=%p\n",
               s_flash.jedec_id, s_flash.regs);
    return BIZ_SUCCESS;
}

void drv_flash_dbg_status(void)
{
    uint32_t boot_sel = *(volatile uint32_t *)FLASH_BOOT_SEL_REG;
    uint32_t win = (boot_sel >> 28) & 0x3U;
    uint32_t ssi_ctrl = *(volatile uint32_t *)FLASH_XIP_CTRL_REG;

    HP_LOGI("[flash] ready=%d JEDEC=0x%06x regs=%p\n",
               s_flash.ready, (unsigned)s_flash.jedec_id, s_flash.regs);
    HP_LOGI("[flash] BOOT_SELECT=0x%08x win=%u ssi_ctrl=0x%08x\n",
               boot_sel, win, ssi_ctrl);
}

uint32_t drv_flash_dbg_ssi_ctrl(int do_write, uint32_t val)
{
    if (do_write)
    {
        *(volatile uint32_t *)FLASH_XIP_CTRL_REG = val;
        flash_xip_barrier();
    }
    return *(volatile uint32_t *)FLASH_XIP_CTRL_REG;
}

void drv_flash_dbg_open_flash(void)
{
    flash_xip_leave_open_flash();
    HP_LOGI("[flash] open_flash: inv + B8 + 98 done, ssi_ctrl=0x%08x\n",
               *(volatile uint32_t *)FLASH_XIP_CTRL_REG);
}

void drv_flash_dbg_bind(void)
{
    static const uint32_t windows[3] = {
        FLASH_SSI_REG0, FLASH_SSI_REG1, FLASH_SSI_REG2,
    };
    uint32_t boot_sel = *(volatile uint32_t *)FLASH_BOOT_SEL_REG;
    uint32_t win = (boot_sel >> 28) & 0x3U;
    uint32_t base;
    uint64_t desc = 0;
    uint32_t attr = 0;
    int mmu_ret;

    if (win > 2U)
        win = 0U;
    base = windows[win];
    if (base == 0U)
        base = FLASH_SSI_PA0_VA;

    mmu_ret = hp232x_mmu_check_l2_block((uint64_t)base,
                                        (windows[win] == 0U) ? 0UL : (uint64_t)base,
                                        &desc, &attr);
    s_flash.regs = (volatile uint32_t *)(uintptr_t)base;
    s_flash.ready = 0;
    s_flash.jedec_id = 0;

    HP_LOGI("[flash] bind BOOT_SELECT=0x%08x win=%u SSI@0x%08lx%s mmu_ret=%d desc=0x%llx attr=%u\n",
               boot_sel, win, (unsigned long)base,
               (windows[win] == 0U) ? " (PA0 via VA alias)" : "",
               mmu_ret, (unsigned long long)desc, attr);
}

int drv_flash_dbg_peek(void)
{
    uint32_t ctrl0, sr;

    if (!s_flash.regs)
    {
        HP_LOGI("[flash] peek: regs not bound (run flash bind first)\n");
        return -1;
    }
    HP_LOGI("[flash] peek SSI@%p ...\n", s_flash.regs);
    ctrl0 = dw_read(s_flash.regs, DW_SPI_CTRL0);
    sr = dw_read(s_flash.regs, DW_SPI_SR);
    HP_LOGI("[flash] peek CTRL0=0x%08x SR=0x%08x\n", ctrl0, sr);
    return 0;
}

int drv_flash_dbg_jedec(void)
{
    int ret;

    if (!s_flash.regs)
    {
        HP_LOGI("[flash] jedec: regs not bound\n");
        return -1;
    }
    ret = flash_probe_jedec();
    if (ret == BIZ_SUCCESS)
        HP_LOGI("[flash] JEDEC=0x%06x ready\n", (unsigned)s_flash.jedec_id);
    else
        HP_LOGI("[flash] JEDEC probe failed ret=%d\n", ret);
    return ret;
}

int drv_flash_dbg_init_now(void)
{
    return drv_flash_bringup();
}

#if defined(RT_USING_SMP) && defined(BSP_FLASH_CPU0_WORKER) && \
    !defined(BSP_FLASH_DIRECT_ON_CALLER)
int drv_flash_worker_start(void)
{
    if (flash_svc_start() != 0)
    {
        BIZ_WARN("flash CPU0 worker start failed\n");
        return -1;
    }
    HP_LOGI("[drv] flash worker on CPU0\n");
    return 0;
}
#elif defined(RT_USING_SMP)
int drv_flash_worker_start(void)
{
    HP_LOGI("[drv] flash DIRECT on caller CPU (no bounce)\n");
    return 0;
}
#endif

int drv_flash_is_known(void)
{
    return s_flash.ready && flash_jedec_is_known(s_flash.jedec_id);
}

uint32_t drv_flash_get_jedec(void)
{
    return s_flash.jedec_id;
}

static int flash_read_local(void *dst, int len, uint32_t addr)
{
    int ret = flash_run_read(dst, len, addr);
    return (ret == 0) ? BIZ_SUCCESS : BIZ_ERR_NORMAL;
}

static int flash_write_local(const void *src, int len, uint32_t addr)
{
    uint32_t erase_start;
    uint32_t erase_len;
    int ret;
    rt_thread_t self = rt_thread_self();
    rt_uint8_t old_prio = 0;
    rt_uint8_t hi_prio = 2; /* above i2c_mcu(3); PP must not be preempted by I2C BH */
    int prio_raised = 0;

    /*
     * WB src: CPU stores (I2C OTA memcpy) leave dirty lines. Host eMMC Load is
     * DMA→DRAM so inv-only was enough; OTA needs clean-before-inv or inv drops
     * the image. Always clean src range before invalidate.
     */
    if (!hp232x_addr_is_normal_nc(src, (size_t)len))
    {
        flash_src_dcache_clean(src, len);
        __asm_invalidate_dcache_all();
    }

    erase_start = (addr / DRV_FLASH_SUBSECTOR_SIZE) * DRV_FLASH_SUBSECTOR_SIZE;
    erase_len = ((uint32_t)len + DRV_FLASH_SUBSECTOR_SIZE - 1U) / DRV_FLASH_SUBSECTOR_SIZE;
    erase_len *= DRV_FLASH_SUBSECTOR_SIZE;

    BIZ_INFO("[flash] erase+program addr=0x%x len=%d (cpu%d src_%s)\n",
             (unsigned)addr, len, (int)rt_hw_cpu_id(),
             hp232x_addr_is_normal_nc(src, (size_t)len) ? "NC" : "WB");

    /*
     * Phase B: MCU polls OTA STATUS over I2C while we program. i2c_mcu (prio 3)
     * would preempt flash worker (prio 5) mid-PP → FIFO underrun / verify fail.
     * Host eMMC path does not poll I2C during FlashWrite.
     */
    if (self)
    {
        /* BSP: read sched priv directly; rt_sched_thread_get_curr_prio is kernel-only. */
        old_prio = RT_SCHED_PRIV(self).current_priority;
        if (old_prio > hi_prio)
        {
            rt_thread_control(self, RT_THREAD_CTRL_CHANGE_PRIORITY, &hi_prio);
            prio_raised = 1;
        }
    }

    ret = flash_run_erase(erase_start, (int)erase_len);
    if (ret == 0)
    {
        if (!hp232x_addr_is_normal_nc(src, (size_t)len))
        {
            flash_src_dcache_clean(src, len);
            __asm_invalidate_dcache_all();
        }
        BIZ_INFO("[flash] erase ok, programming...\n");
        ret = flash_run_write(src, len, addr);
    }
    if (ret == 0)
        BIZ_INFO("[flash] program ok\n");

    if (prio_raised && self)
        rt_thread_control(self, RT_THREAD_CTRL_CHANGE_PRIORITY, &old_prio);

    if (ret != 0)
        return BIZ_ERR_FLASH_WRITE_CHECK;

    return BIZ_SUCCESS;
}

static int flash_erase_local(uint32_t addr, int len)
{
    int ret = flash_run_erase(addr, len);
    return (ret == 0) ? BIZ_SUCCESS : BIZ_ERR_NORMAL;
}

int drv_flash_read(void *dst, int len, uint32_t addr)
{
    if (!s_flash.ready || !dst || len <= 0)
        return BIZ_ERR_NORMAL;

#if defined(RT_USING_SMP) && defined(BSP_FLASH_CPU0_WORKER) && \
    !defined(BSP_FLASH_DIRECT_ON_CALLER)
    /* Bounce unless already the worker (avoid deadlock; CPU0 callers like
     * i2cota reuse the 8KB worker stack instead of needing another 8KB). */
    if (s_flash_svc.worker_ready && rt_thread_self() != s_flash_svc.worker)
        return flash_svc_call(FLASH_JOB_READ, RT_NULL, dst, len, addr);
#endif
    return flash_read_local(dst, len, addr);
}

int drv_flash_write(const void *src, int len, uint32_t addr)
{
    if (!s_flash.ready || !src || len <= 0)
        return BIZ_ERR_NORMAL;

#if defined(RT_USING_SMP) && defined(BSP_FLASH_CPU0_WORKER) && \
    !defined(BSP_FLASH_DIRECT_ON_CALLER)
    if (s_flash_svc.worker_ready && rt_thread_self() != s_flash_svc.worker)
        return flash_svc_call(FLASH_JOB_WRITE, src, RT_NULL, len, addr);
#endif
    return flash_write_local(src, len, addr);
}

int drv_flash_erase(uint32_t addr, int len)
{
    if (!s_flash.ready || len <= 0)
        return BIZ_ERR_NORMAL;

#if defined(RT_USING_SMP) && defined(BSP_FLASH_CPU0_WORKER) && \
    !defined(BSP_FLASH_DIRECT_ON_CALLER)
    if (s_flash_svc.worker_ready && rt_thread_self() != s_flash_svc.worker)
        return flash_svc_call(FLASH_JOB_ERASE, RT_NULL, RT_NULL, len, addr);
#endif
    return flash_erase_local(addr, len);
}

/*
 * Direct SSI access on a chosen CPU (no bounce). For Core1 SError triage:
 *   p1 raw MMIO SR/CTRL0
 *   p2 EPROMREAD RDSR
 *   p3 TMOD_TO WREN
 *   p4 EPROMREAD RDSR (expect WEL)
 *
 * Target CPU must not call rt_kprintf (console lock + msh waiter → SMP deadlock).
 * Results are stored lock-free; caller CPU prints.
 */
static volatile int s_ssi_probe_phase;
static volatile int s_ssi_probe_ret;
static volatile int s_ssi_probe_done;
static volatile int s_ssi_probe_cpu;
static volatile uint32_t s_ssi_probe_ctrl0;
static volatile uint32_t s_ssi_probe_sr_reg;
static volatile uint8_t s_ssi_probe_rdsr;
static volatile uint8_t s_ssi_probe_rdsr_wel;

static void flash_ssi_probe_body(void *param)
{
    uint8_t sr = 0xFF;
    uint8_t cmd = NOR_CMD_WREN;
    (void)param;

    if (!s_flash.regs)
    {
        s_ssi_probe_phase = 0;
        s_ssi_probe_ret = -10;
        s_ssi_probe_done = 1;
        return;
    }

    s_ssi_probe_cpu = (int)rt_hw_cpu_id();
    s_ssi_probe_ret = 0;

    /* p1: pure SSI MMIO — hang/SError here ⇒ interconnect / map issue */
    s_ssi_probe_phase = 1;
    s_ssi_probe_ctrl0 = dw_read(s_flash.regs, DW_SPI_CTRL0);
    s_ssi_probe_sr_reg = dw_read(s_flash.regs, DW_SPI_SR);

    flash_xip_spi_mode();

    /* p2: EPROMREAD RDSR (lyn-sfc style) */
    s_ssi_probe_phase = 2;
    if (flash_nor_read_reg(NOR_CMD_RDSR, &sr, 1) != 0)
    {
        s_ssi_probe_ret = -1;
        goto out;
    }
    s_ssi_probe_rdsr = sr;

    /* p3: TMOD_TO WREN — historic SError site was TX PIO */
    s_ssi_probe_phase = 3;
    flash_ahb_cfg(FLASH_SPEED_NORMAL, SPI_TMOD_TX, 0);
    if (flash_tx_only(&cmd, 1) != 0)
    {
        flash_close();
        s_ssi_probe_ret = -2;
        goto out;
    }
    flash_close();

    /* p4: confirm WEL via EPROMREAD */
    s_ssi_probe_phase = 4;
    if (flash_nor_read_reg(NOR_CMD_RDSR, &sr, 1) != 0)
    {
        s_ssi_probe_ret = -3;
        goto out;
    }
    s_ssi_probe_rdsr_wel = sr;

    /* p5: one PAGE program + readback @ TEST_ADDR — historic SError on TX path */
    s_ssi_probe_phase = 5;
    {
        uint8_t wr[16];
        uint8_t rd[16];
        int i;

        for (i = 0; i < 16; i++)
            wr[i] = (uint8_t)(0xA0 + i);
        if (flash_run_erase(DRV_FLASH_TEST_ADDR, DRV_FLASH_SUBSECTOR_SIZE) != 0)
        {
            s_ssi_probe_ret = -4;
            goto out;
        }
        if (flash_run_write(wr, 16, DRV_FLASH_TEST_ADDR) != 0)
        {
            s_ssi_probe_ret = -5;
            goto out;
        }
        if (flash_run_read(rd, 16, DRV_FLASH_TEST_ADDR) != 0)
        {
            s_ssi_probe_ret = -6;
            goto out;
        }
        for (i = 0; i < 16; i++)
        {
            if (rd[i] != wr[i])
            {
                s_ssi_probe_ret = -7;
                goto out;
            }
        }
    }

    s_ssi_probe_phase = 6;
    s_ssi_probe_ret = 0;

out:
    s_ssi_probe_done = 1;
}

static void flash_ssi_probe_report(void)
{
    HP_LOGI("[flash] ssi_probe cpu=%d phase=%d ret=%d\n",
               s_ssi_probe_cpu, s_ssi_probe_phase, s_ssi_probe_ret);
    if (s_ssi_probe_phase >= 1)
        HP_LOGI("[flash] ssi_probe p1 CTRL0=0x%08x SR=0x%08x\n",
                   s_ssi_probe_ctrl0, s_ssi_probe_sr_reg);
    if (s_ssi_probe_phase >= 2 && s_ssi_probe_ret != -1)
        HP_LOGI("[flash] ssi_probe p2 EPROMREAD RDSR=0x%02x\n",
                   s_ssi_probe_rdsr);
    if (s_ssi_probe_phase >= 3 && s_ssi_probe_ret != -2)
        HP_LOGI("[flash] ssi_probe p3 TMOD_TO WREN ok\n");
    if (s_ssi_probe_phase >= 4 && s_ssi_probe_ret != -3)
        HP_LOGI("[flash] ssi_probe p4 RDSR=0x%02x WEL=%d\n",
                   s_ssi_probe_rdsr_wel,
                   (s_ssi_probe_rdsr_wel & 0x02U) ? 1 : 0);
    if (s_ssi_probe_phase >= 5 && s_ssi_probe_ret != -4 &&
        s_ssi_probe_ret != -5 && s_ssi_probe_ret != -6 && s_ssi_probe_ret != -7)
        HP_LOGI("[flash] ssi_probe p5 erase+PP+verify @0x%x ok\n",
                   (unsigned)DRV_FLASH_TEST_ADDR);
    if (s_ssi_probe_phase >= 6 && s_ssi_probe_ret == 0)
        HP_LOGI("[flash] ssi_probe PASS cpu=%d\n", s_ssi_probe_cpu);
    else if (s_ssi_probe_ret != 0)
        HP_LOGI("[flash] ssi_probe FAIL cpu=%d ret=%d\n",
                   s_ssi_probe_cpu, s_ssi_probe_ret);
}

int drv_flash_ssi_cpu_probe(int cpu_id)
{
#ifdef RT_USING_SMP
    rt_thread_t t;
    int i;

    if (!s_flash.ready || !s_flash.regs)
        return BIZ_ERR_NORMAL;
    if (cpu_id < 0 || cpu_id >= RT_CPUS_NR)
        return BIZ_ERR_CLI_PARAM;

    s_ssi_probe_done = 0;
    s_ssi_probe_phase = 0;
    s_ssi_probe_ret = 0;
    s_ssi_probe_cpu = -1;
    s_ssi_probe_ctrl0 = 0;
    s_ssi_probe_sr_reg = 0;
    s_ssi_probe_rdsr = 0;
    s_ssi_probe_rdsr_wel = 0;

    if (cpu_id == (int)rt_hw_cpu_id())
    {
        flash_ssi_probe_body(RT_NULL);
        flash_ssi_probe_report();
        return s_ssi_probe_ret;
    }

    /*
     * Priority MUST stay below emmc_biz (prio 2). Do not raise past biz —
     * probe is diagnostic only; during query_task busy-poll it may TIMEOUT.
     */
    t = rt_thread_create("fl_prb", flash_ssi_probe_body, RT_NULL,
                         4096, 3, 5);
    if (!t)
        return BIZ_ERR_NORMAL;

    if (rt_thread_control(t, RT_THREAD_CTRL_BIND_CPU, (void *)(rt_ubase_t)cpu_id) != RT_EOK)
    {
        HP_LOGI("[flash] ssi_probe bind CPU%d fail\n", cpu_id);
        rt_thread_delete(t);
        return BIZ_ERR_NORMAL;
    }

    rt_thread_startup(t);
    /* erase+PP may take seconds; allow up to 60s */
    for (i = 0; i < 6000; i++)
    {
        if (s_ssi_probe_done)
            break;
        rt_thread_mdelay(10);
    }

    if (!s_ssi_probe_done)
    {
        HP_LOGI("[flash] ssi_probe TIMEOUT cpu=%d last_phase=%d (SError or not scheduled)\n",
                   cpu_id, s_ssi_probe_phase);
        return BIZ_ERR_NORMAL;
    }
    flash_ssi_probe_report();
    return s_ssi_probe_ret;
#else
    (void)cpu_id;
    flash_ssi_probe_body(RT_NULL);
    flash_ssi_probe_report();
    return s_ssi_probe_ret;
#endif
}
