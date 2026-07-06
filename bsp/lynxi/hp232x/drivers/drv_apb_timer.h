/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HP232X DesignWare APB timer driver for system tick.
 */

#ifndef __DRV_APB_TIMER_H__
#define __DRV_APB_TIMER_H__

#include <rtthread.h>
#include <interrupt.h>

#ifndef HP232X_APB_TIMER_TICK_ID
#define HP232X_APB_TIMER_TICK_ID    0U
#endif

int rt_hw_apb_timer_init(void);
void rt_hw_apb_timer_stop(void);
extern volatile rt_uint32_t hp232x_apb_timer_isr_count;

#define HP232X_APB_TIMER_TICK_IRQ   (TIMER_IRQ_START + HP232X_APB_TIMER_TICK_ID)

#endif /* __DRV_APB_TIMER_H__ */
