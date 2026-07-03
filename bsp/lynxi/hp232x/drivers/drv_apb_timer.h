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

int rt_hw_apb_timer_init(void);
void rt_hw_apb_timer_stop(void);

#endif /* __DRV_APB_TIMER_H__ */
