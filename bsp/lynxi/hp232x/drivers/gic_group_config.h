/*
 * HP232X GIC中断组别配置头文件
 *
 * Copyright (c) 2025 lynxi
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __GIC_GROUP_CONFIG_H__
#define __GIC_GROUP_CONFIG_H__

#include <rtthread.h>

/**
 * 配置Timer中断组别为Group1 Non-Secure
 *
 * Timer30是PPI中断，需要在GIC Redistributor的SGI_base配置
 *
 * @param redist_base: Redistributor基地址
 */
void hp232x_config_timer_interrupt_group(rt_uint64_t redist_base);

/**
 * 配置UART中断组别为Group1 Non-Secure
 *
 * UART57是SPI中断，需要在GIC Distributor配置
 *
 * @param dist_base: Distributor基地址
 */
void hp232x_config_uart_interrupt_group(rt_uint64_t dist_base);

void hp232x_config_i2c_interrupt_group(rt_uint64_t dist_base);

/**
 * 初始化HP232X中断组别配置
 *
 * 在Non-secure EL1启动后调用，确保Timer和UART中断配置为Group1 NS
 */
void hp232x_init_interrupt_groups(void);

/**
 * 验证中断组别配置
 *
 * 用于调试，检查Timer和UART中断的组别设置是否正确
 */
void hp232x_verify_interrupt_groups(void);

#endif /* __GIC_GROUP_CONFIG_H__ */