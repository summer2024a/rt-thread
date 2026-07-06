/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HP232X DesignWare APB timer driver for system tick.
 */

#include <rtthread.h>
#include <rthw.h>
#include <interrupt.h>
#include "lynxi.h"
#include "drv_apb_timer.h"

#define DBG_TAG "drv.apb_timer"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define DW_TIMER_LOAD_COUNT(id)     (0x00 + (id) * 0x14)
#define DW_TIMER_CURRENT_VALUE(id)  (0x04 + (id) * 0x14)
#define DW_TIMER_CONTROL(id)        (0x08 + (id) * 0x14)
#define DW_TIMER_EOI(id)            (0x0c + (id) * 0x14)
#define DW_TIMER_INT_STATUS(id)     (0x10 + (id) * 0x14)

#define DW_TIMER_CONTROL_ENABLE     (1U << 0)
#define DW_TIMER_CONTROL_MODE       (1U << 1)  /* 1: user-defined periodic mode */
#define DW_TIMER_CONTROL_INT_MASK   (1U << 2)  /* 1: interrupt masked */

#ifndef HP232X_APB_TIMER_CLOCK
#define HP232X_APB_TIMER_CLOCK      50000000U
#endif

static rt_uint32_t apb_timer_read(rt_uint32_t offset)
{
    return *((volatile rt_uint32_t *)((rt_ubase_t)STIMER_BASE + offset));
}

static void apb_timer_write(rt_uint32_t offset, rt_uint32_t value)
{
    *((volatile rt_uint32_t *)((rt_ubase_t)STIMER_BASE + offset)) = value;
}

static void apb_timer_disable(rt_uint32_t id)
{
    rt_uint32_t control;

    control = apb_timer_read(DW_TIMER_CONTROL(id));
    control &= ~DW_TIMER_CONTROL_ENABLE;
    control |= DW_TIMER_CONTROL_INT_MASK;
    apb_timer_write(DW_TIMER_CONTROL(id), control);
    (void)apb_timer_read(DW_TIMER_EOI(id));
}

volatile rt_uint32_t hp232x_apb_timer_isr_count;

static void apb_timer_isr(int vector, void *parameter)
{
    rt_uint32_t id = (rt_uint32_t)(rt_ubase_t)parameter;

    RT_UNUSED(vector);

    hp232x_apb_timer_isr_count++;

    /* Reading TxEOI clears the DesignWare APB timer interrupt. */
    (void)apb_timer_read(DW_TIMER_EOI(id));
    rt_tick_increase();
}

int rt_hw_apb_timer_init(void)
{
    rt_uint32_t load;
    rt_uint32_t id = HP232X_APB_TIMER_TICK_ID;
    int vector = HP232X_APB_TIMER_TICK_IRQ;

    RT_ASSERT(id < CONFIG_TIMER_NUM);
    RT_ASSERT(vector < MAX_HANDLERS);

    load = HP232X_APB_TIMER_CLOCK / RT_TICK_PER_SECOND;
    RT_ASSERT(load > 0);

    apb_timer_disable(id);
    apb_timer_write(DW_TIMER_LOAD_COUNT(id), load);
    (void)apb_timer_read(DW_TIMER_EOI(id));

    rt_hw_interrupt_install(vector, apb_timer_isr, (void *)(rt_ubase_t)id, "apb_tick");
    rt_hw_interrupt_set_triger_mode(vector, IRQ_MODE_TRIG_LEVEL);
    rt_hw_interrupt_set_priority(vector, 0xa0);
    rt_hw_interrupt_umask(vector);

    /* Enable periodic mode and leave interrupt unmasked. */
    apb_timer_write(DW_TIMER_CONTROL(id),
                    DW_TIMER_CONTROL_ENABLE | DW_TIMER_CONTROL_MODE);

    LOG_I("APB timer%d system tick: irq=%d clock=%uHz load=%u",
          id, vector, HP232X_APB_TIMER_CLOCK, load);

    return RT_EOK;
}

void rt_hw_apb_timer_stop(void)
{
    apb_timer_disable(HP232X_APB_TIMER_TICK_ID);
    rt_hw_interrupt_mask(HP232X_APB_TIMER_TICK_IRQ);
}
