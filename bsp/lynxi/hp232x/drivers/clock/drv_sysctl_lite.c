/*
 * Lynxi Lite sysctl init for HP232X — SPL pll_init() + fabric/UART gates.
 * Align lynxi-uboot pll_init(), he200 drv_sysctl_lite.c, Linux clk-lynxi-lite.c.
 */
#include <rtthread.h>

#include "drv_sysctl_lite.h"
#include "lynxi.h"

#define SYSCTL_REG32(off) (*(volatile rt_uint32_t *)((rt_uintptr_t)CPR_BASE + (rt_uint32_t)(off)))

#define CPR_BOOT_SELECT_OFF         0x64u
#define CPR_PLL_LOCK_STATUS_OFF     0xd4u
#define CPR_PLL_LOCK_MASK           0xffffu
#define CPR_PLL_LOCK_VALUE          0x3f3f3u
#define CPR_BOOT_SELECT_PLL_MASK    0xffffffd0u

rt_uint32_t lynxi_sysctl_lite_boot_select_before;
rt_uint32_t lynxi_sysctl_lite_boot_select_after;

rt_uint32_t lynxi_sysctl_lite_reg_read(rt_uint32_t reg_off)
{
    return SYSCTL_REG32(reg_off);
}

void lynxi_sysctl_lite_gate_enable(rt_uint32_t reg_off, rt_uint32_t bit)
{
    rt_uint32_t v;

    v = SYSCTL_REG32(reg_off);
    v |= (1u << bit);
    SYSCTL_REG32(reg_off) = v;
}

static void _sysctl_gate_on(rt_uint32_t reg_off, rt_uint32_t bit)
{
    lynxi_sysctl_lite_gate_enable(reg_off, bit);
}

/*
 * SPL board_init_f() calls pll_init() before RT-Thread; BL22 skips SPL.
 * Switch BOOT_SELECT muxes from xin24m to PLL so CNTPCT runs at 31.25MHz
 * (1500M/6/8) instead of 500kHz (24M/6/8).
 */
static void _sysctl_pll_boot_select(void)
{
    rt_uint32_t v;
    rt_uint32_t timeout = 1000000u;

    while (timeout--)
    {
        v = SYSCTL_REG32(CPR_PLL_LOCK_STATUS_OFF) & CPR_PLL_LOCK_MASK;
        if (v == CPR_PLL_LOCK_VALUE)
        {
            break;
        }
    }

    lynxi_sysctl_lite_boot_select_before = SYSCTL_REG32(CPR_BOOT_SELECT_OFF);

    v = SYSCTL_REG32(CPR_BOOT_SELECT_OFF);
    v &= CPR_BOOT_SELECT_PLL_MASK;
    SYSCTL_REG32(CPR_BOOT_SELECT_OFF) = v;

    lynxi_sysctl_lite_boot_select_after = SYSCTL_REG32(CPR_BOOT_SELECT_OFF);

    __asm__ volatile("dsb sy" ::: "memory");
}

void lynxi_sysctl_lite_init(void)
{
    _sysctl_pll_boot_select();

    /* cpu_timer gate — arch timer clock (CPR+0x68 bit8, Linux LITE_TIMER) */
    _sysctl_gate_on(0x68u, 8u);

    /*
     * fabric_pclk2: UART/Timer/SPI APB side (lynchip-lite-cpr.dtsi)
     * fabric_aclk6: fabric bus for several peripherals
     * fabric_hclk2 + ssi_boot: Boot SSI / sfc_nor1 (Linux lynchip-lite-cpr.dtsi)
     */
    _sysctl_gate_on(0x6cu, 5u); /* LITE_FABRIC_ACLK6 */
    _sysctl_gate_on(0x6cu, 7u); /* LITE_FABRIC_HCLK2 — Boot SSI AHB */
    _sysctl_gate_on(0x6cu, 9u); /* LITE_FABRIC_PCLK2 */
    _sysctl_gate_on(0xc4u, 1u); /* LITE_SSI_BOOT — ssi_boot_clk */

    _sysctl_gate_on(0xb4u, 1u); /* UART0 sclk */
    _sysctl_gate_on(0xb8u, 1u); /* UART1 sclk */
    _sysctl_gate_on(0xbcu, 1u); /* WDT */
    _sysctl_gate_on(0x90u, 1u); /* GPIO debounce */
    _sysctl_gate_on(0x90u, 2u); /* GPIO intr */
}
