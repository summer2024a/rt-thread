/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024/01/11     xxx          ARMv8 version
 */

#include <rthw.h>
#include <rtthread.h>

static volatile rt_uint64_t time_elapsed = 0;
static volatile rt_uint64_t tick_cycles = 0;

/* ARMv8 通用定时器寄存器 */
#define CNTP_CTL_EL0    S3_6_C14_C2_1   /* 物理定时器控制寄存器 */
#define CNTP_CVAL_EL0   S3_6_C14_C2_2   /* 物理定时器比较值寄存器 */
#define CNTP_TVAL_EL0   S3_6_C14_C2_3   /* 物理定时器计时值寄存器 */
#define CNTFRQ_EL0      S3_6_C14_C0_0   /* 定时器频率寄存器 */

/**
 * 读取当前计数器值
 */
rt_inline rt_uint64_t armv8_get_cntpct(void)
{
    rt_uint64_t cnt;
    __asm__ __volatile__(
        "mrs %0, cntpct_el0"
        : "=r"(cnt)
    );
    return cnt;
}

/**
 * 读取定时器频率
 */
rt_inline rt_uint64_t armv8_get_cntfrq(void)
{
    rt_uint64_t freq;
    __asm__ __volatile__(
        "mrs %0, cntfrq_el0"
        : "=r"(freq)
    );
    return freq;
}

/**
 * 设置定时器比较值
 */
rt_inline void armv8_set_cntp_cval(rt_uint64_t cval)
{
    __asm__ __volatile__(
        "msr cntp_cval_el0, %0"
        :
        : "r"(cval)
    );
}

/**
 * 使能/禁用定时器
 */
rt_inline void armv8_cntp_ctl_enable(rt_bool_t enable)
{
    rt_uint32_t ctl;

    if (enable)
    {
        ctl = 0x1;  /* 使能定时器 */
    }
    else
    {
        ctl = 0x0;  /* 禁用定时器 */
    }

    __asm__ __volatile__(
        "msr cntp_ctl_el0, %0"
        :
        : "r"(ctl)
    );
    __asm__ __volatile__("isb");
}

/**
 * 定时器中断处理函数
 */
int rt_hw_tick_isr(void)
{
    /* 增加系统 tick */
    rt_tick_increase();

    /* 设置下一次中断时间 */
    armv8_set_cntp_cval(armv8_get_cntpct() + tick_cycles);

    return 0;
}

/**
 * 初始化系统 tick
 */
int rt_hw_tick_init(void)
{
    rt_uint64_t freq;

    /* 获取定时器频率 */
    freq = armv8_get_cntfrq();

    /* 计算每个 tick 的计数值 */
    tick_cycles = freq / RT_TICK_PER_SECOND;

    rt_kprintf("[rt_hw_tick_init] freq: %d tick_cycles: %d\n",
               (rt_uint32_t)freq, (rt_uint32_t)tick_cycles);

    /* 禁用定时器 */
    armv8_cntp_ctl_enable(RT_FALSE);

    /* 设置初始比较值 */
    armv8_set_cntp_cval(armv8_get_cntpct() + tick_cycles);

    /* 使能定时器 */
    armv8_cntp_ctl_enable(RT_TRUE);

    return 0;
}

/**
 * 微秒级延时
 *
 * @param us 延时时间，单位微秒
 */
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

/**
 * 获取系统 tick 频率
 */
rt_uint32_t rt_hw_tick_get_freq(void)
{
    return (rt_uint32_t)armv8_get_cntfrq();
}