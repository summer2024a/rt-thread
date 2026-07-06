/*
 * PMON thread - monitor system tick and APB timer IRQ activity.
 */

#include <rtthread.h>
#include <rthw.h>
#include "drv_apb_timer.h"

#ifdef RT_BSP_PMON_TEST

static void pmon_gic_thread(void *parameter)
{
    int count = 0;
    rt_tick_t last_tick = 0;
    rt_uint32_t last_isr = 0;

    RT_UNUSED(parameter);

    rt_kprintf("X\n");  /* pmon thread started */

    while (1)
    {
        rt_tick_t now_tick = rt_tick_get();
        rt_uint32_t now_isr = hp232x_apb_timer_isr_count;
        rt_tick_t delta_tick = now_tick - last_tick;
        rt_uint32_t delta_isr = now_isr - last_isr;

        count++;

        /* Every 500ms: expect ~50 ticks and ~50 ISR at RT_TICK_PER_SECOND=100 */
        if (count % 1 == 0)
        {
            rt_kprintf("[PMON] #%d tick=%u (+%u) isr=%u (+%u)\n",
                       count, (unsigned)now_tick, (unsigned)delta_tick,
                       (unsigned)now_isr, (unsigned)delta_isr);
        }

        last_tick = now_tick;
        last_isr = now_isr;

        rt_thread_mdelay(500);

        if (count >= 20)
        {
            rt_kprintf("[PMON] Done (20 samples)\n");
            break;
        }
    }
}

int pmon_gic_init(void)
{
    rt_thread_t tid = rt_thread_create("pmon_gic",
                                       pmon_gic_thread,
                                       RT_NULL,
                                       4096,
                                       15,
                                       10);

    if (tid)
    {
        rt_thread_startup(tid);
        rt_kprintf("[PMON] Thread created (APB timer tick monitor)\n");
        return RT_EOK;
    }

    rt_kprintf("[PMON] Failed to create thread\n");
    return -RT_ERROR;
}

INIT_APP_EXPORT(pmon_gic_init);

#endif /* RT_BSP_PMON_TEST */
