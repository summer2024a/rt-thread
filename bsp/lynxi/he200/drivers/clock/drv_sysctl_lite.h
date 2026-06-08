/*
 * Lynxi Lite (lynchip-lite) sysctl 时钟门控 — 对齐 Linux drivers/clk/lynxi/clk-lynxi-lite.c
 * 与 CPR/复位同源基址 sysctl@12500000（见 lynchip-lite-cpr.dtsi）。
 */
#ifndef __DRV_SYSCTL_LITE_H__
#define __DRV_SYSCTL_LITE_H__

#include <rtthread.h>

/* 与 Linux lynchip_lite_cpr_gmac_config() / EVB gmac-ctrl-reg 一致 */
#define LYNXI_SYSCTL_GMAC_CTRL_REG_OFF   (0x8cu)
#define LYNXI_SYSCTL_GMAC_CTRL_1000M     (0x66fu)
#define LYNXI_SYSCTL_GMAC_CTRL_100M      (0x65fu)
#define LYNXI_SYSCTL_GMAC_CTRL_10M       (0x64fu)

void lynxi_sysctl_lite_init(void);

/* Linux dwc_qos_probe 等价：fabric + LITE_GMAC_R 脉冲 + aclk/phy_ref/hclk gate */
void lynxi_sysctl_lite_gmac_probe_clocks(void);

/* 仅写 CPR 侧 GMAC 控制字（可重复调用） */
void lynxi_sysctl_lite_gmac_ctrl_set(rt_uint32_t value);

/* Linux lynchip_lite_cpr_gmac_config(1000M)：含 RGMII mux，DMA SWR 前需要 */
void lynxi_sysctl_lite_gmac_cpr_apply_1000m(void);

/* 打开单个 gate：reg_off 为相对 CPR_BASE 的字节偏移（与 Linux _GATE 一致） */
void lynxi_sysctl_lite_gate_enable(rt_uint32_t reg_off, rt_uint32_t bit);

#endif /* __DRV_SYSCTL_LITE_H__ */
