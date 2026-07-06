/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2025-06-17     lynxi          hp232x BSP — simplified main for IRAM-only system
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

int main(int argc, char** argv)
{
    rt_kprintf("\nHi, this is RT-Thread!\n");

#ifdef RT_USING_SMP
    if (RT_CPUS_NR > 1)
    {
        rt_ubase_t cpu;

        for (cpu = 1; cpu < RT_CPUS_NR; cpu++)
        {
            int retry = 300;

            while (retry-- > 0 && rt_cpu_index(cpu)->current_thread == RT_NULL)
            {
                rt_thread_mdelay(10);
            }

            if (rt_cpu_index(cpu)->current_thread != RT_NULL)
            {
                rt_kprintf("[SMP] CPU%lu ready (idle=%p)\n",
                           cpu, rt_cpu_index(cpu)->current_thread);
            }
            else
            {
                rt_kprintf("[SMP] CPU%lu NOT online after wait\n", cpu);
            }
        }
    }
#endif

    rt_thread_yield();

    return 0;
}
