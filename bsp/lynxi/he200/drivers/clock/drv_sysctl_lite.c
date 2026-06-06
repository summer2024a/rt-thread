/*
 * Lynxi Lite sysctl 时钟门控 — 对齐 Linux clk-lynxi-lite.c 中 composite gate 偏移/位号。
 *
 * 参考：
 * - drivers/clk/lynxi/clk-lynxi-lite.c  lite_composite_clks / lite_gate_clks
 * - arch/arm64/boot/dts/lynxi/lynchip-lite-cpr.dtsi 各 IP clocks = <&sysctl ...>
 */
#include <rtthread.h>
#include <rtdef.h>

#include "drv_sysctl_lite.h"
#include "lynxi.h"
#include "tick.h"

#define SYSCTL_REG32(off) (*(volatile rt_uint32_t *)((rt_uintptr_t)CPR_BASE + (rt_uint32_t)(off)))

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

void lynxi_sysctl_lite_gmac_ctrl_set(rt_uint32_t value)
{
    SYSCTL_REG32(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF) = value;
}

static void _lynxi_gmac_reset_pulse(void)
{
    rt_uint32_t v;

    v = SYSCTL_REG32(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF);
    SYSCTL_REG32(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF) = v & ~1u;
    rt_hw_us_delay(100);
    v = SYSCTL_REG32(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF);
    SYSCTL_REG32(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF) = v | 1u;
    rt_hw_us_delay(1000);
}

void lynxi_sysctl_lite_gmac_probe_clocks(void)
{
    /*
     * 对齐 Zephyr/Linux：dwc_qos_probe 开 aclk→phy_ref→hclk；
     * probe 阶段不写 0x66f（链路建立后 lynchip_lite_cpr_gmac_config 再写）。
     */
    _sysctl_gate_on(0x6cu, 5u);
    _sysctl_gate_on(0x6cu, 9u);
    _lynxi_gmac_reset_pulse();
    _sysctl_gate_on(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF, 1u); /* LITE_GMAC_ACLK */
    _sysctl_gate_on(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF, 9u); /* LITE_ETH_PHY */
    _sysctl_gate_on(LYNXI_SYSCTL_GMAC_CTRL_REG_OFF, 2u); /* LITE_GMAC_HCLK */
}

void lynxi_sysctl_lite_init(void)
{
    /*
     * fabric_pclk2：UART/Timer/SPI 等 APB 侧第二路时钟（见 cpr.dtsi uart0/1 clocks 含 LITE_FABRIC_PCLK2）
     * fabric_aclk6：部分外设 fabric 总线
     */
    _sysctl_gate_on(0x6cu, 5u); /* LITE_FABRIC_ACLK6 */
    _sysctl_gate_on(0x6cu, 9u); /* LITE_FABRIC_PCLK2 */

    /* fabric_aclk5 / fabric_hclk1 — DMA（lynchip-lite-cpr.dtsi &dma clocks） */
#ifdef BSP_USING_DW_AXI_DMA
    _sysctl_gate_on(0x6cu, 4u); /* LITE_FABRIC_ACLK5 */
    _sysctl_gate_on(0x6cu, 6u); /* LITE_FABRIC_HCLK1 */
#endif

    /* I2C HS 时钟门（0x94 bit 3~5） */
#ifdef BSP_USING_I2C0
    _sysctl_gate_on(0x94u, 3u);
#endif
#ifdef BSP_USING_I2C1
    _sysctl_gate_on(0x94u, 4u);
#endif
#ifdef BSP_USING_I2C2
    _sysctl_gate_on(0x94u, 5u);
#endif
#ifdef BSP_USING_I2C3
    /* Linux 使用 LITE_PERIPH_SMB，gate 0x98 bit 1；与 HS I2C 不同 */
    _sysctl_gate_on(0x98u, 1u);
#endif

    /* UART（periph_uart0/1_sclk） */
    _sysctl_gate_on(0xb4u, 1u);
    _sysctl_gate_on(0xb8u, 1u);

    /* WDT */
    _sysctl_gate_on(0xbcu, 1u);

    /* GPIO debounce / intr（GPIO 子系统常用） */
    _sysctl_gate_on(0x90u, 1u);
    _sysctl_gate_on(0x90u, 2u);

#ifdef BSP_USING_SDIO
    _sysctl_gate_on(0x88u, 1u); /* emmc_aclk */
    _sysctl_gate_on(0x88u, 2u); /* emmc_hclk */
    _sysctl_gate_on(0x88u, 3u); /* emmc_cclk */
#endif

#ifdef BSP_USING_PCIE
    _sysctl_gate_on(0x84u, 2u); /* pcie_mst */
    _sysctl_gate_on(0x84u, 3u); /* pcie_slv */
    _sysctl_gate_on(0x84u, 4u); /* pcie_dbi_aclk */
    _sysctl_gate_on(0x84u, 5u); /* pcie_x2p_aclk */
    _sysctl_gate_on(0x84u, 7u); /* pcie_apb_clk */
#endif

    /*
     * 不在 board init 写 0x66f；GMAC 在 rt_hw_eth_init 调 gmac_probe_clocks，
     * 链路速率在 lynxi_gmac_set_ctrl_by_speed() 写（对齐 lynchip_lite_cpr_gmac_config）。
     */
}

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>

static void sysctl_r(int argc, char **argv)
{
    unsigned long off;

    if (argc < 2)
    {
        rt_kprintf("usage: sysctl_r <offset_hex>\n");
        return;
    }
    off = strtoul(argv[1], RT_NULL, 16);
    if (off >= 0x1000u)
    {
        rt_kprintf("offset must be < 0x1000\n");
        return;
    }
    rt_kprintf("CPR+0x%03lx = 0x%08x\n", off, (unsigned int)SYSCTL_REG32((rt_uint32_t)off));
}
MSH_CMD_EXPORT(sysctl_r, read CPR/sysctl reg e.g. sysctl_r 6c);

static void sysctl_gate(int argc, char **argv)
{
    unsigned long off;
    unsigned long bit;

    if (argc < 3)
    {
        rt_kprintf("usage: sysctl_gate <offset_hex> <bit_dec>\n");
        return;
    }
    off = strtoul(argv[1], RT_NULL, 16);
    bit = strtoul(argv[2], RT_NULL, 10);
    if (bit > 31u)
    {
        rt_kprintf("bit 0..31\n");
        return;
    }
    lynxi_sysctl_lite_gate_enable((rt_uint32_t)off, (rt_uint32_t)bit);
    rt_kprintf("CPR+0x%lx |= (1<<%lu)\n", off, bit);
}
MSH_CMD_EXPORT(sysctl_gate, set one gate bit e.g. sysctl_gate 6c 9);
#endif /* RT_USING_FINSH */
