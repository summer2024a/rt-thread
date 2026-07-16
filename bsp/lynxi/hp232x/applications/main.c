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
#include "biz_log.h"
#include <rtdevice.h>
#include <string.h>
#include <board.h>
#include <rthw.h>
#include "biz_subsys.h"
#include "drv_flash.h"
#include "hp232x_mmu.h"

#if defined(RT_USING_SMP) && defined(BSP_USING_HP232X_SMP_BIND_TEST)
static volatile rt_uint32_t smp_bind_counter;
static rt_thread_t smp_bind_worker;

static void smp_bind_worker_entry(void *parameter)
{
    RT_UNUSED(parameter);

    HP_LOGI("[SMP] worker running on CPU%d\n", rt_hw_cpu_id());

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
        HP_LOGI("[SMP] bind test: create worker failed\n");
        return;
    }

    err = rt_thread_control(smp_bind_worker, RT_THREAD_CTRL_BIND_CPU, (void *)1);
    if (err != RT_EOK)
    {
        HP_LOGI("[SMP] bind test: bind CPU1 failed (%d)\n", err);
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
        HP_LOGI("[SMP] bind test PASS: worker counter %u -> %u (CPU1)\n", c0, c1);
    }
    else
    {
        HP_LOGI("[SMP] bind test FAIL: worker stuck at %u (check IPI/GIC on CPU1)\n", c0);
    }
}
#endif /* RT_USING_SMP && BSP_USING_HP232X_SMP_BIND_TEST */

#if defined(RT_USING_SMP)
#ifdef BSP_SMP_EARLY_MARK
#define SMP_EARLY_PUTC(c) early_putc_direct(c)
#else
#define SMP_EARLY_PUTC(c) do { } while (0)
#endif

/**
 * Busy-wait for secondary CPUs. After rt_hw_secondary_cpu_up(), secondary may
 * hold _cpus_lock — must NOT use rt_kprintf here until online (or given up).
 * With BSP_SMP_EARLY_MARK: '.' while waiting, 'Y' online / 'N!' timeout.
 */
int hp232x_smp_wait_secondaries(void)
{
    rt_ubase_t cpu;
    int offline = 0;

    for (cpu = 1; cpu < RT_CPUS_NR; cpu++)
    {
        int retry = 500000; /* ~5s @ 10us */
        struct rt_cpu *c = rt_cpu_index(cpu);
        int ok = 0;

        while (retry-- > 0)
        {
            rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, c, sizeof(*c));
            if (c->current_thread != RT_NULL)
            {
                ok = 1;
                break;
            }
            if ((retry % 5000) == 0)
            {
                hp232x_smp_release_cpu((int)cpu);
                SMP_EARLY_PUTC('.');
            }
            rt_hw_us_delay(10);
        }

        if (ok)
        {
            SMP_EARLY_PUTC('Y');
            /* Secondary past scheduler_start — lock released, kprintf OK */
            rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, c, sizeof(*c));
            HP_LOGI("[SMP] CPU%lu ready (idle=%p)\n",
                       cpu, c->current_thread);
        }
        else
        {
            SMP_EARLY_PUTC('N');
            SMP_EARLY_PUTC('!');
            offline++;
        }
    }
    return offline;
}

/*
 * Flash cold: jumper leaves ssi_ctrl=ba. Leave-XIP + flush tables BEFORE
 * release so CPU1 MMU sees coherent page tables (UART path usually OK without).
 */
static void hp232x_smp_release_secondaries(void)
{
    drv_flash_dbg_open_flash();
    hp232x_mmu_flush_tables();
    rt_hw_secondary_cpu_up();
    SMP_EARLY_PUTC('s');
}

#ifdef BSP_SMP_DEFER_SECONDARY
static int cmd_smp(int argc, char **argv)
{
    int offline;

    if (argc < 2)
    {
        rt_kprintf("smp start  — leave-XIP + release CPU1 + wait\n");
        rt_kprintf("smp release — leave-XIP + release only (no wait)\n");
        return -RT_ERROR;
    }

    if (strcmp(argv[1], "start") != 0 && strcmp(argv[1], "release") != 0)
    {
        rt_kprintf("smp start | smp release\n");
        return -RT_ERROR;
    }

    hp232x_smp_release_secondaries();

    if (strcmp(argv[1], "release") == 0)
        return 0;

    offline = hp232x_smp_wait_secondaries();
#if defined(BSP_USING_HP232X_SMP_BIND_TEST)
    if (!offline)
        smp_bind_selftest();
#endif
    return offline;
}
MSH_CMD_EXPORT_ALIAS(cmd_smp, smp, smp start|release);
#endif /* BSP_SMP_DEFER_SECONDARY */
#endif /* RT_USING_SMP */

int main(int argc, char** argv)
{
    /* Banner first while still CPU0-only (or secondaries already idle). */
    HP_LOGI("\nHi, this is RT-Thread! (build stamp " __DATE__ " " __TIME__ ")\n");

#if defined(RT_USING_SMP) && (RT_CPUS_NR > 1)
#ifdef BSP_SMP_DEFER_SECONDARY
    /* 命令行启动：msh smp start */
    HP_LOGI("[main] SMP deferred — msh: smp start\n");
#else
    /* 自启动：leave-XIP + flush + release + wait */
    hp232x_smp_release_secondaries();
    (void)hp232x_smp_wait_secondaries();
#if defined(BSP_USING_HP232X_SMP_BIND_TEST)
    smp_bind_selftest();
#endif
#endif
#endif

#ifdef BSP_FLASH_DEFER_INIT
    HP_LOGI("[main] flash deferred — msh: flash init | flash worker\n");
#else
    if (drv_flash_bringup() != 0)
        HP_LOGI("[main] flash bringup failed (SSI/JEDEC)\n");
#endif

#ifdef BSP_I2C_DEFER
    HP_LOGI("[main] i2c deferred — msh: i2c start\n");
#endif

#ifdef BSP_BIZ_SKIP_THREADS
    HP_LOGI("[main] biz deferred — msh: biz start [cpu] | heart_beat\n");
#else
    if (biz_emmc_biz_start() != 0)
        HP_LOGI("[main] emmc_biz start failed\n");
#endif

    rt_thread_yield();
    return 0;
}
