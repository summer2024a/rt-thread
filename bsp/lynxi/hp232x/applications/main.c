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

#if defined(RT_USING_SMP) && defined(BSP_USING_HP232X_SMP_BIND_TEST)
static volatile rt_uint32_t smp_bind_counter;
static rt_thread_t smp_bind_worker;

static void smp_bind_worker_entry(void *parameter)
{
    RT_UNUSED(parameter);

    rt_kprintf("[SMP] worker running on CPU%d\n", rt_hw_cpu_id());

    while (1)
    {
        smp_bind_counter++;
        rt_thread_mdelay(100);
    }
}

static void smp_bind_selftest(void)
{
    rt_err_t err;
    rt_uint32_t c0, c1;
    int retry;

    smp_bind_worker = rt_thread_create("smpw", smp_bind_worker_entry, RT_NULL,
                                       1024, 25, 10);
    if (smp_bind_worker == RT_NULL)
    {
        rt_kprintf("[SMP] bind test: create worker failed\n");
        return;
    }

    err = rt_thread_control(smp_bind_worker, RT_THREAD_CTRL_BIND_CPU, (void *)1);
    if (err != RT_EOK)
    {
        rt_kprintf("[SMP] bind test: bind CPU1 failed (%d)\n", err);
        rt_thread_delete(smp_bind_worker);
        smp_bind_worker = RT_NULL;
        return;
    }

    c0 = smp_bind_counter;
    rt_thread_startup(smp_bind_worker);

    for (retry = 0; retry < 30; retry++)
    {
        rt_thread_mdelay(100);
        if (smp_bind_counter > c0)
        {
            break;
        }
    }

    c1 = smp_bind_counter;
    if (c1 > c0)
    {
        rt_kprintf("[SMP] bind test PASS: worker counter %u -> %u (CPU1)\n", c0, c1);
    }
    else
    {
        rt_kprintf("[SMP] bind test FAIL: worker stuck at %u (check IPI/GIC on CPU1)\n", c0);
    }
}
#endif /* RT_USING_SMP && BSP_USING_HP232X_SMP_BIND_TEST */

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

#if defined(BSP_USING_HP232X_SMP_BIND_TEST)
        smp_bind_selftest();
#endif
    }
#endif

    rt_thread_yield();

    return 0;
}
