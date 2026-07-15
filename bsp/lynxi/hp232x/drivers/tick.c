/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024/01/11     xxx          ARMv8 version
 * 2025-06-17     lynxi        hp232x BSP — tick using ARMv8 generic timer
 */

#include <rthw.h>
#include "biz_log.h"
#include <rtthread.h>

static volatile rt_uint64_t time_elapsed = 0;
static volatile rt_uint64_t tick_cycles = 0;

#define CNTP_CTL_EL0    S3_6_C14_C2_1
#define CNTP_CVAL_EL0   S3_6_C14_C2_2
#define CNTFRQ_EL0      S3_6_C14_C0_0

rt_inline rt_uint64_t armv8_get_cntpct(void)
{
    rt_uint64_t cnt;
    __asm__ __volatile__(
        "mrs %0, cntpct_el0"
        : "=r"(cnt)
    );
    return cnt;
}

rt_inline rt_uint64_t armv8_get_cntfrq(void)
{
    rt_uint64_t freq;
    __asm__ __volatile__(
        "mrs %0, cntfrq_el0"
        : "=r"(freq)
    );
    return freq;
}

rt_inline void armv8_set_cntp_cval(rt_uint64_t cval)
{
    __asm__ __volatile__(
        "msr cntp_cval_el0, %0"
        :
        : "r"(cval)
    );
}

rt_inline void armv8_cntp_ctl_enable(rt_bool_t enable)
{
    rt_uint32_t ctl;

    if (enable)
    {
        ctl = 0x1;
    }
    else
    {
        ctl = 0x0;
    }

    __asm__ __volatile__(
        "msr cntp_ctl_el0, %0"
        :
        : "r"(ctl)
    );
    __asm__ __volatile__("isb");
}

int rt_hw_tick_isr(void)
{
    rt_tick_increase();
    armv8_set_cntp_cval(armv8_get_cntpct() + tick_cycles);
    return 0;
}

int rt_hw_tick_init(void)
{
    rt_uint64_t freq;

    freq = armv8_get_cntfrq();
    tick_cycles = freq / RT_TICK_PER_SECOND;

    HP_LOGI("[rt_hw_tick_init] freq: %d tick_cycles: %d\n",
               (rt_uint32_t)freq, (rt_uint32_t)tick_cycles);

    armv8_cntp_ctl_enable(RT_FALSE);
    armv8_set_cntp_cval(armv8_get_cntpct() + tick_cycles);
    armv8_cntp_ctl_enable(RT_TRUE);

    return 0;
}

void rt_hw_us_delay(rt_uint32_t us)
{
    rt_uint64_t start_time;
    rt_uint64_t end_time;
    rt_uint64_t freq;

    freq = armv8_get_cntfrq();
    start_time = armv8_get_cntpct();
    end_time = start_time + us * (freq / 1000000);

    do {
        __asm__ __volatile__("isb");
    } while (armv8_get_cntpct() < end_time);
}

rt_uint32_t rt_hw_tick_get_freq(void)
{
    return (rt_uint32_t)armv8_get_cntfrq();
}
