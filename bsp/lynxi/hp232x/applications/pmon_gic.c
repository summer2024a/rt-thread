/*
 * PMON thread - monitor system tick (and optional APB timer ISR activity).
 */

#include <rtthread.h>
#include <rthw.h>
#ifdef BSP_USING_APB_TIMER_AS_TICK
#include "drv_apb_timer.h"
#endif

#ifdef RT_BSP_PMON_TEST

#if defined(RT_USING_SMP) && defined(BSP_USING_HP232X_PMON_BIND_CPU1)
#define PMON_BIND_CPU           1
#define PMON_EXPECT_CPU         1
#else
#define PMON_BIND_CPU           (-1)
#define PMON_EXPECT_CPU         (-1)
#endif

static void pmon_gic_thread(void *parameter)
{
    int count = 0;
    rt_tick_t last_tick = 0;
    rt_uint32_t last_isr = 0;
    rt_ubase_t cpu = rt_hw_cpu_id();

    RT_UNUSED(parameter);

    rt_kprintf("[PMON][CPU%lu] thread started (expect CPU%d)\n",
               cpu, PMON_EXPECT_CPU);

    while (1)
    {
        rt_tick_t now_tick = rt_tick_get();
#ifdef BSP_USING_APB_TIMER_AS_TICK
        rt_uint32_t now_isr = hp232x_apb_timer_isr_count;
#else
        rt_uint32_t now_isr = 0;
#endif
        rt_tick_t delta_tick = now_tick - last_tick;
        rt_uint32_t delta_isr = now_isr - last_isr;

        count++;
        cpu = rt_hw_cpu_id();

        /* Every 500ms: expect ~50 ticks and ~50 ISR at RT_TICK_PER_SECOND=100 */
        rt_kprintf("[PMON][CPU%lu] #%d tick=%u (+%u) isr=%u (+%u)\n",
                   cpu, count, (unsigned)now_tick, (unsigned)delta_tick,
                   (unsigned)now_isr, (unsigned)delta_isr);

        last_tick = now_tick;
        last_isr = now_isr;

        rt_thread_mdelay(500);

        if (count >= 20)
        {
            cpu = rt_hw_cpu_id();
            if (PMON_EXPECT_CPU >= 0 && cpu == (rt_ubase_t)PMON_EXPECT_CPU)
            {
                rt_kprintf("[PMON][CPU%lu] PASS: ran 20 samples on CPU%d\n",
                           cpu, PMON_EXPECT_CPU);
            }
            else if (PMON_EXPECT_CPU >= 0)
            {
                rt_kprintf("[PMON][CPU%lu] FAIL: expected CPU%d\n",
                           cpu, PMON_EXPECT_CPU);
            }
            else
            {
                rt_kprintf("[PMON][CPU%lu] Done (20 samples)\n", cpu);
            }
            break;
        }
    }
}

int pmon_gic_init(void)
{
    rt_thread_t tid;
    rt_err_t err;

    tid = rt_thread_create("pmon_gic",
                           pmon_gic_thread,
                           RT_NULL,
                           4096,
                           15,
                           10);

    if (tid == RT_NULL)
    {
        rt_kprintf("[PMON] Failed to create thread\n");
        return -RT_ERROR;
    }

#if PMON_BIND_CPU >= 0
    err = rt_thread_control(tid, RT_THREAD_CTRL_BIND_CPU, (void *)(rt_ubase_t)PMON_BIND_CPU);
    if (err != RT_EOK)
    {
        rt_kprintf("[PMON] bind CPU%d failed (%d), use default CPU\n",
                   PMON_BIND_CPU, err);
    }
    else
    {
        rt_kprintf("[PMON] thread bound to CPU%d (init on CPU%lu)\n",
                   PMON_BIND_CPU, rt_hw_cpu_id());
    }
#endif

    rt_thread_startup(tid);
#ifdef BSP_USING_APB_TIMER_AS_TICK
    rt_kprintf("[PMON] thread started (APB timer tick monitor)\n");
#else
    rt_kprintf("[PMON] thread started (arch timer tick monitor)\n");
#endif
    return RT_EOK;
}

INIT_APP_EXPORT(pmon_gic_init);

#endif /* RT_BSP_PMON_TEST */
