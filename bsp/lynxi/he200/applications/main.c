/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#ifdef BSP_USING_GMAC
extern int rt_hw_eth_init(void);

#ifdef RT_USING_SMP
static void gmac_wait_secondary_cpus_online(void)
{
    rt_ubase_t cpu;

    for (cpu = 1; cpu < RT_CPUS_NR; cpu++)
    {
        int retry = 200;

        while (retry-- > 0 && rt_cpu_index(cpu)->current_thread == RT_NULL)
        {
            rt_thread_mdelay(10);
        }
    }
}
#endif

static void gmac_init_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
#ifdef RT_USING_SMP
    /*
     * 移植 GMAC 前 SMP 正常：勿在从核尚未 scheduler_start 时动 GMAC 时钟/PHY。
     * components.c 已调 rt_hw_secondary_cpu_up()，此处等待各核 current_thread 就绪。
     */
    gmac_wait_secondary_cpus_online();
#endif
    rt_hw_eth_init();
}
#endif

int main(int argc, char** argv)
{
#ifdef BSP_USING_GMAC
    rt_thread_t tid = rt_thread_create("gmaci", gmac_init_thread_entry, RT_NULL,
                                         8192, 25, 10);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
    }
    else
    {
        rt_kprintf("gmac: init thread create failed, fallback sync init\n");
        rt_hw_eth_init();
    }
#endif
    rt_kprintf("Hi, this is RT-Thread!!\n");
    rt_thread_yield();
    return 0;
}
