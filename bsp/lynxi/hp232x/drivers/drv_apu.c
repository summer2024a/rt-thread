/*
 * drv_apu.c — KA200 APU (lyn_apu) control.
 * Ported from hp640_arm/common/spl/spl_cmd.c.
 */

#include <rtthread.h>
#include "drv_apu.h"
#include "hp232x_mmu.h"
#include "biz_log.h"
#include "tick.h"

#define CPR_BASE                    0x12500000UL
#define CPR_BOOT_SELECT             (CPR_BASE + 0x64UL)
#define CPR_CLOCK_SW_CTRL           (CPR_BASE + 0xCCUL)
#define CPR_APU_PLL_CONFIG0         (CPR_BASE + 0x3CUL)
#define CPR_APU_PLL_CONFIG2         (CPR_BASE + 0x44UL)
#define CPR_PLL_LOCK_STATUS         (CPR_BASE + 0xD4UL)
#define CPR_PLL_LOCK_VALUE          0x3F3FU

static inline uint32_t cpr_readl(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void cpr_writel(uint32_t val, uint32_t addr)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static inline uint32_t apu_readl(uint32_t offset)
{
    return *(volatile uint32_t *)(uintptr_t)(HP232X_APU_BASE + offset);
}

static inline void apu_writel(uint32_t val, uint32_t offset)
{
    *(volatile uint32_t *)(uintptr_t)(HP232X_APU_BASE + offset) = val;
}

void drv_apu_reset(void)
{
    uint32_t val = cpr_readl(DRV_APU_CTRL_REG);

    val &= ~DRV_APU_CTRL_RST;
    cpr_writel(val, DRV_APU_CTRL_REG);
    val |= DRV_APU_CTRL_RST;
    cpr_writel(val, DRV_APU_CTRL_REG);
}

int drv_apu_enable(int enable)
{
    uint32_t apu_ctrl = cpr_readl(DRV_APU_CTRL_REG);

    if (enable)
    {
        apu_ctrl |= DRV_APU_CTRL_CLK;
        apu_ctrl |= DRV_APU_CTRL_RST;
    }
    else
    {
        apu_ctrl &= ~DRV_APU_CTRL_CLK;
    }

    cpr_writel(apu_ctrl, DRV_APU_CTRL_REG);
    return 0;
}

int drv_apu_pll_set(uint32_t pll_cfg2)
{
    int timeout = 100000;

    drv_apu_enable(1);

    cpr_writel(cpr_readl(CPR_CLOCK_SW_CTRL) | 0x104U, CPR_CLOCK_SW_CTRL);
    cpr_writel(cpr_readl(CPR_BOOT_SELECT) | 0x8U, CPR_BOOT_SELECT);

    cpr_writel(cpr_readl(CPR_APU_PLL_CONFIG0) & ~1U, CPR_APU_PLL_CONFIG0);
    cpr_writel(pll_cfg2, CPR_APU_PLL_CONFIG2);
    cpr_writel(cpr_readl(CPR_APU_PLL_CONFIG0) | 1U, CPR_APU_PLL_CONFIG0);

    while (cpr_readl(CPR_PLL_LOCK_STATUS) != CPR_PLL_LOCK_VALUE)
    {
        if (--timeout == 0)
        {
            BIZ_WARN("APU PLL lock timeout\n");
            break;
        }
        rt_hw_us_delay(10);
    }

    cpr_writel(cpr_readl(CPR_CLOCK_SW_CTRL) & 0xFFFFFEFBU, CPR_CLOCK_SW_CTRL);
    cpr_writel(cpr_readl(CPR_BOOT_SELECT) & 0xFFFFFFF7U, CPR_BOOT_SELECT);

    drv_apu_enable(0);
    return 0;
}

uint32_t drv_apu_read32(uint32_t offset)
{
    return apu_readl(offset);
}

void drv_apu_write32(uint32_t offset, uint32_t value)
{
    apu_writel(value, offset);
}

int drv_apu_read_status(void)
{
    return (int)(apu_readl(DRV_APU_STAT_OFFSET) & 0x01U);
}

int drv_apu_init(void)
{
    drv_apu_enable(1);
    drv_apu_reset();
    drv_apu_enable(0);
    BIZ_INFO("APU init done\n");
    return 0;
}
