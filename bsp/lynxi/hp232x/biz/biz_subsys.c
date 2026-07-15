/*
 * biz_subsys.c — Business layer init: config/log/ipc orchestration and worker threads.
 */
#include <rtthread.h>
#include <rthw.h>
#include "biz_subsys.h"
#include "biz_log.h"
#include "biz_config.h"
#include "biz_ipc.h"
#include "drv_flash.h"
#include "drv_apu.h"
#include "drv_i2c.h"
#include "drv_emmc.h"
#include "biz_emmc.h"
#include "biz_modules.h"
#include "biz_exec_handlers.h"
#include "board.h"
#if defined(RT_USING_SMP)
#include <gicv3.h>
extern rt_uint64_t rt_cpu_mpidr_table[];
#endif

#define I2C_BH_STACK_SIZE       2048
#define MCU_ERR_STACK_SIZE      1024
#define EMMC_BIZ_STACK_SIZE     8192

static rt_thread_t s_emmc_biz_tid;
static int s_emmc_inited;

static void emmc_biz_trampoline(void *param)
{
    HP_LOGI("[drv] emmc_biz entry CPU%d\n", rt_hw_cpu_id());
    biz_emmc_biz_entry(param);
}

static rt_thread_t start_thread(const char *name, void (*entry)(void *),
                                rt_uint8_t priority, rt_uint32_t stack)
{
    rt_thread_t t = rt_thread_create(name, entry, RT_NULL, stack, priority, 10);

    if (t)
    {
        rt_thread_startup(t);
        BIZ_INFO("%s thread started\n", name);
    }
    else
    {
        BIZ_ERROR("Failed to create %s thread\n", name);
    }

    return t;
}

static int biz_emmc_hw_ensure(void)
{
    if (s_emmc_inited)
        return 0;
    if (drv_emmc_init() != 0)
    {
        BIZ_ERROR("drv_emmc_init failed\n");
        return -1;
    }
    s_emmc_inited = 1;
    return 0;
}

static int biz_init(void)
{
    HP_LOGI("[biz] init: log/config/ipc\n");
    biz_log_init();
    biz_config_init();
    biz_ipc_init();

#ifdef BSP_FLASH_DEFER_INIT
    HP_LOGI("[biz] hw: flash deferred (msh: flash init / worker)\n");
#else
    HP_LOGI("[biz] hw: flash\n");
    if (drv_flash_init() != 0)
        BIZ_WARN("SPI flash init failed\n");
#endif

#ifndef BSP_BIZ_SKIP_THREADS
    HP_LOGI("[biz] hw: emmc\n");
    if (biz_emmc_hw_ensure() != 0)
        BIZ_WARN("eMMC init failed\n");
#else
    HP_LOGI("[biz] hw: emmc deferred (msh: biz start / heart_beat)\n");
#endif

    HP_LOGI("[biz] init: done\n");
    return 0;
}
INIT_DEVICE_EXPORT(biz_init);

static int s_i2c_workers_up;

static int biz_i2c_workers_start(void)
{
    if (s_i2c_workers_up)
    {
        HP_LOGI("[biz] i2c workers already up\n");
        return 0;
    }

    HP_LOGI("[biz] worker: i2c\n");
    if (drv_i2c_init() != 0)
    {
        BIZ_WARN("MCU I2C init failed\n");
        return -1;
    }

    if (!drv_i2c_is_ready())
    {
        BIZ_WARN("MCU I2C not ready after init\n");
        return -1;
    }

    BIZ_INFO("I2C MCU slave @ 0x%02x (IRQ mode)\n", drv_i2c_mcu_get_addr());
    start_thread("i2c_mcu", drv_i2c_bh_entry, 3, I2C_BH_STACK_SIZE);
    start_thread("mcu_err", biz_mcu_err_bh_entry, 4, MCU_ERR_STACK_SIZE);
    s_i2c_workers_up = 1;
    return 0;
}

int biz_i2c_ensure(void)
{
    return biz_i2c_workers_start();
}

static int biz_worker_init(void)
{
#ifdef BSP_BIZ_SKIP_THREADS
    HP_LOGI("[biz] emmc_biz deferred (BSP_BIZ_SKIP_THREADS) — msh: biz start\n");
#else
    HP_LOGI("[biz] worker: apu\n");
    drv_apu_init();

#if defined(RT_USING_SMP) && !defined(BSP_FLASH_DEFER_INIT)
    if (drv_flash_worker_start() != 0)
        BIZ_WARN("flash worker start failed\n");
#endif

#ifdef BIZ_MOD_PCIE
    HP_LOGI("[biz] worker: pcie\n");
    drv_pcie_boot_init();
#endif
#endif /* !BSP_BIZ_SKIP_THREADS */

#ifdef BSP_I2C_DEFER
    HP_LOGI("[biz] i2c deferred (BSP_I2C_DEFER) — msh: i2c start\n");
#else
    if (biz_i2c_workers_start() != 0)
        BIZ_WARN("i2c workers start failed\n");
#endif

#ifndef BSP_BIZ_SKIP_THREADS
    HP_LOGI("[biz] worker: ready\n");
    BIZ_INFO("Business workers ready on CPU0\n");
#endif
    return 0;
}
INIT_ENV_EXPORT(biz_worker_init);

int biz_flash_worker_ensure(void)
{
#if defined(RT_USING_SMP)
    return drv_flash_worker_start();
#else
    return 0;
#endif
}

int biz_emmc_biz_start_on_cpu(int cpu)
{
    rt_thread_t t;
    rt_err_t err;

    if (cpu < 0 || cpu >= RT_CPUS_NR)
    {
        BIZ_ERROR("emmc_biz: bad cpu %d (0..%d)\n", cpu, RT_CPUS_NR - 1);
        return -1;
    }

    if (s_emmc_biz_tid != RT_NULL)
    {
        HP_LOGI("[biz] emmc_biz already running (tid=%p)\n", s_emmc_biz_tid);
        return 0;
    }

    if (biz_emmc_hw_ensure() != 0)
        return -1;

#ifdef BSP_BIZ_SKIP_THREADS
    /* APU used by HB/stress; harmless if already called */
    drv_apu_init();
#endif

    t = rt_thread_create("emmc_biz", emmc_biz_trampoline, RT_NULL,
                         EMMC_BIZ_STACK_SIZE, 2, 10);
    if (t == RT_NULL)
    {
        BIZ_ERROR("emmc_biz create failed (stack=%u, heap tight?)\n",
                  (unsigned)EMMC_BIZ_STACK_SIZE);
        return -1;
    }

#if defined(RT_USING_SMP)
    err = rt_thread_control(t, RT_THREAD_CTRL_BIND_CPU, (void *)(rt_ubase_t)cpu);
    if (err != RT_EOK)
    {
        BIZ_ERROR("emmc_biz bind CPU%d failed (%d)\n", cpu, err);
        rt_thread_delete(t);
        return -1;
    }
    HP_LOGI("[biz] emmc_biz bound to CPU%d\n", cpu);

    if (cpu != 0)
    {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, rt_cpu_mpidr_table,
                             sizeof(rt_cpu_mpidr_table[0]) * RT_CPUS_NR);
        arm_gic_sgi_affinity_reset();
    }
#else
    (void)err;
    if (cpu != 0)
        HP_LOGI("[biz] WARN: SMP off, ignore cpu=%d\n", cpu);
#endif

    rt_thread_startup(t);
    s_emmc_biz_tid = t;

#if defined(RT_USING_SMP)
    if (cpu != 0)
    {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, t, sizeof(*t));
        if (t->stack_addr && t->stack_size)
            rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, t->stack_addr, t->stack_size);
        hp232x_kick_cpu(cpu);
    }
#endif

    BIZ_INFO("emmc_biz thread started\n");
    return 0;
}

int biz_emmc_biz_start(void)
{
#if defined(RT_USING_SMP) && defined(BSP_BIZ_EMMC_ON_CPU1)
    return biz_emmc_biz_start_on_cpu(1);
#else
    return biz_emmc_biz_start_on_cpu(0);
#endif
}

int biz_emmc_biz_is_running(void)
{
    return s_emmc_biz_tid != RT_NULL;
}

int biz_emmc_hw_is_ready(void)
{
    return s_emmc_inited;
}
