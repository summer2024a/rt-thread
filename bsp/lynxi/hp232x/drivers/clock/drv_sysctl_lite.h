/*
 * Lynxi Lite sysctl clock gates for HP232X (align Linux clk-lynxi-lite.c).
 */
#ifndef __DRV_SYSCTL_LITE_H__
#define __DRV_SYSCTL_LITE_H__

#include <rtthread.h>

void lynxi_sysctl_lite_init(void);
void lynxi_sysctl_lite_gate_enable(rt_uint32_t reg_off, rt_uint32_t bit);
rt_uint32_t lynxi_sysctl_lite_reg_read(rt_uint32_t reg_off);
extern rt_uint32_t lynxi_sysctl_lite_boot_select_before;
extern rt_uint32_t lynxi_sysctl_lite_boot_select_after;

#endif /* __DRV_SYSCTL_LITE_H__ */
