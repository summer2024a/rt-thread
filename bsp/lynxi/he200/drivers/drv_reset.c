#include <rtthread.h>
#include <stdlib.h>
#include "lynxi.h"
#include "tick.h"
#include "drv_reset.h"

#define LYNXI_BOOT_SELECT_OFFSET        0x64U /* boot select register */
#define LYNXI_SYS_SW_RESET_OFFSET       0x60U /* system software reset register */
#define LYNXI_BOOT_SSI_CONTROL_OFFSET   0xC4U

#define LYNXI_SYSTEM_RESET_VALUE        0x0U
#define LYNXI_BOOT_SSI_CLK              0x3U
#define LYNXI_PAD_PLL_CLK               0x3FU

#ifdef BSP_USING_RESET_CTRL

struct lynxi_reset_desc
{
    rt_uint32_t reg_offset;
    rt_uint32_t bit;
};

static const struct lynxi_reset_desc _reset_map[] =
{
    /* LITE_GMAC_R=1120 → CPR 0x8c bit0（勿用 0x88 bit0，那是 LITE_EMMC_R） */
    [LYNXI_RESET_ETH]  = {0x8c, 0},
    [LYNXI_RESET_I2C0] = {0x90, 0},
    [LYNXI_RESET_I2C1] = {0x90, 1},
    [LYNXI_RESET_I2C2] = {0x90, 2},
    [LYNXI_RESET_I2C3] = {0x90, 3},
    [LYNXI_RESET_RTC]  = {0x94, 0},
    [LYNXI_RESET_DMA]  = {0x98, 0},
};

static rt_err_t _lynxi_reset_set(enum lynxi_reset_line line, rt_bool_t assert)
{
    volatile rt_uint32_t *reg;
    rt_uint32_t mask;
    rt_base_t level;

    if ((rt_uint32_t)line >= (sizeof(_reset_map) / sizeof(_reset_map[0])))
    {
        return -RT_EINVAL;
    }

    reg = (volatile rt_uint32_t *)(rt_uintptr_t)(CPR_BASE + _reset_map[line].reg_offset);
    mask = (1U << _reset_map[line].bit);

    /*
     * reset-lynchip-lite.c：assert=清 bit，deassert=置 bit（与常规 “写 1 复位” 相反）。
     */
    level = rt_hw_interrupt_disable();
    if (assert)
    {
        *reg &= ~mask;
    }
    else
    {
        *reg |= mask;
    }
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

rt_err_t lynxi_reset_assert(enum lynxi_reset_line line)
{
    return _lynxi_reset_set(line, RT_TRUE);
}

rt_err_t lynxi_reset_deassert(enum lynxi_reset_line line)
{
    return _lynxi_reset_set(line, RT_FALSE);
}

rt_err_t lynxi_reset_pulse(enum lynxi_reset_line line, rt_uint32_t delay_ms)
{
    rt_err_t ret;

    ret = lynxi_reset_assert(line);
    if (ret != RT_EOK)
    {
        return ret;
    }

    /*
     * CPR IP reset hold time: busy-wait so this is safe before the scheduler
     * starts (e.g. INIT_BOARD_EXPORT). This is not KA200 full-chip reset.
     */
    if (delay_ms > 0U)
    {
        rt_hw_us_delay(delay_ms * 1000U);
    }
    return lynxi_reset_deassert(line);
}

#else

rt_err_t lynxi_reset_assert(enum lynxi_reset_line line)
{
    RT_UNUSED(line);
    return RT_EOK;
}

rt_err_t lynxi_reset_deassert(enum lynxi_reset_line line)
{
    RT_UNUSED(line);
    return RT_EOK;
}

rt_err_t lynxi_reset_pulse(enum lynxi_reset_line line, rt_uint32_t delay_ms)
{
    RT_UNUSED(line);
    RT_UNUSED(delay_ms);
    return RT_EOK;
}

#endif /* BSP_USING_RESET_CTRL */

void rt_hw_cpu_reset(void)
{
    volatile rt_uint32_t *boot_sel_reg;
    volatile rt_uint32_t *sys_sw_reset_reg;
    volatile rt_uint32_t *boot_ssi_ctrl_reg;
    rt_uint32_t boot_sel_value;
    rt_uint32_t sys_sw_reset_value;
    rt_uint32_t boot_ssi_ctrl_value;
    rt_base_t level;

    boot_sel_reg = (volatile rt_uint32_t *)(rt_uintptr_t)(CPR_BASE + LYNXI_BOOT_SELECT_OFFSET);
    sys_sw_reset_reg = (volatile rt_uint32_t *)(rt_uintptr_t)(CPR_BASE + LYNXI_SYS_SW_RESET_OFFSET);
    boot_ssi_ctrl_reg = (volatile rt_uint32_t *)(rt_uintptr_t)(CPR_BASE + LYNXI_BOOT_SSI_CONTROL_OFFSET);

    level = rt_hw_interrupt_disable();

    /* Keep the previous CPR values for rollback if reset does not take effect. */
    boot_ssi_ctrl_value = *boot_ssi_ctrl_reg;
    boot_sel_value = *boot_sel_reg;
    sys_sw_reset_value = *sys_sw_reset_reg;

    *boot_ssi_ctrl_reg = LYNXI_BOOT_SSI_CLK;
    *boot_sel_reg = LYNXI_PAD_PLL_CLK;
    *sys_sw_reset_reg = LYNXI_SYSTEM_RESET_VALUE;

    /* If hardware reset succeeds, code below should never execute. */
    rt_hw_us_delay(1000U * 1000U);

    *sys_sw_reset_reg = sys_sw_reset_value;
    *boot_sel_reg = boot_sel_value;
    *boot_ssi_ctrl_reg = boot_ssi_ctrl_value;
    rt_hw_interrupt_enable(level);

    while (1)
    {
        RT_ASSERT(0);
    }
}

#ifdef RT_USING_FINSH
#include <finsh.h>

static void reboot(void)
{
    rt_kprintf("rebooting...\n");
    rt_hw_cpu_reset();
}
MSH_CMD_EXPORT(reboot, reboot system (chip-level reset));

static void reset_pulse(int argc, char **argv)
{
    int id;
    int ms = 1;
    if (argc < 2)
    {
        rt_kprintf("usage: reset_pulse <id> [ms]\n");
        return;
    }
    id = atoi(argv[1]);
    if (argc > 2)
    {
        ms = atoi(argv[2]);
    }
    if (lynxi_reset_pulse((enum lynxi_reset_line)id, (rt_uint32_t)ms) == RT_EOK)
    {
        rt_kprintf("reset pulse ok, id=%d\n", id);
    }
    else
    {
        rt_kprintf("reset pulse failed, id=%d\n", id);
    }
}
MSH_CMD_EXPORT(reset_pulse, pulse reset line: reset_pulse <id> [ms]);
#endif
