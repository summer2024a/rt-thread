/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 * 2021-12-28     GuEe-GUI       add smp support
 * 2023-03-28     WangXiaoyao    sync works & memory layout fixups
 *                               code formats
 */
#define DBG_TAG "board"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>
#include <smp_call.h>
#include <rthw.h>
#include <rtthread.h>
#include <mm_aspace.h>
#include <cpuport.h>

#include "board.h"
#include "drv_uart.h"

#include "cp15.h"
#include <mmu.h>
#include <cpuport.h>
#include <interrupt.h>
#include <mm_page.h>
#include <gic.h>
#include <gicv3.h>
#include <gtimer.h>

#ifdef RT_USING_SMART
#include <lwp_arch.h>
#endif

extern size_t MMUTable[];
extern void *system_vectors;

size_t gpio_base_addr = GPIO_BASE_ADDR;

size_t uart_base_addr = UART_BASE;

size_t gic_base_addr = GIC_V2_BASE;

size_t arm_timer_base = ARM_TIMER_BASE;

size_t stimer_base_addr = STIMER_BASE;

size_t mmc2_base_addr   = MMC2_BASE_ADDR;

size_t wdt_base_addr = WDT_BASE;

size_t pcie_ep_base_addr = PCIE_EP_BASE;

#ifdef RT_USING_SMART
struct mem_desc platform_mem_desc[] = {
    {KERNEL_VADDR_START, KERNEL_VADDR_START + 0x0fffffff, (rt_size_t)ARCH_MAP_FAILED, NORMAL_MEM}
};
#else
struct mem_desc platform_mem_desc[] = {
    /* IRAM  */
    {0x04000000UL, 0x040FFFFFUL, 0x04000000UL, NORMAL_MEM},
    /*CPU_SUB_SLV + PERIPH_SLV + SYS_CTRL_SLV + DDR_CFG_SLV + \
            VPU_CFG_SLV + V52_CFG_SLV + PCIE_DBI_SLV + PCIE_X2_SLV 224MB*/
    // {0x08000000UL, 0x08000000UL + 0x14000000UL - 1, 0x08000000UL, DEVICE_MEM},
    {INTC_BASE, INTC_BASE + 0x14000000UL - 1, INTC_BASE, DEVICE_MEM},
    // {INTC_BASE, INTC_BASE + 0x200000 - 1, INTC_BASE, DEVICE_MEM},
    /* 0x000800000000 ~ 0x0008FFFFFFFF is for cacheable memory */
    {MEM_PADDR_START, MEM_PADDR_START+MEM_CACHE_SZ - 1, MEM_PADDR_START, NORMAL_MEM},
    /* 0x000900000000 ~ 0x0009FFFFFFFF is for cacheable memory */
    {MEM_PADDR_START+MEM_CACHE_SZ, MEM_PADDR_START+MEM_CACHE_SZ+MEM_NOCACHE_SZ - 1, MEM_PADDR_START+MEM_CACHE_SZ, NORMAL_NOCACHE_MEM},
};

#endif

#ifdef BSP_USING_GICV3
rt_uint64_t rt_cpu_mpidr_table[] =
{
#if RT_CPUS_NR > 1
    [0] = 0x0UL, /* CPU0 */
    [1] = 0x1UL, /* CPU1 */
    [2] = 0x2UL, /* CPU2 */
    [3] = 0x3UL, /* CPU3 */
    [4] = 0x100UL, /* CPU4 */
    [5] = 0x101UL, /* CPU5 */
    [6] = 0x102UL, /* CPU6 */
    [7] = 0x103UL, /* CPU7 */
#else
    [RT_CPUS_NR] = 0,
#endif
};
#endif

const rt_uint32_t platform_mem_desc_size = sizeof(platform_mem_desc)/sizeof(platform_mem_desc[0]);

void idle_wfi(void)
{
    asm volatile ("wfi");
}

static void system_vectors_init(void)
{
    rt_hw_set_current_vbar((rt_ubase_t)&system_vectors);
}

/**
 * This function will initialize board
 */

extern size_t MMUTable[];

rt_region_t init_page_region = {
    PAGE_START,
    PAGE_END,
};

/**
 *  Initialize the Hardware related stuffs. Called from rtthread_startup()
 *  after interrupt disabled.
 */
void rt_hw_board_init(void)
{
    rt_hw_earlycon_ioremap_early();

    /* io device remap */
#ifdef RT_USING_SMART
    rt_hw_mmu_map_init(&rt_kernel_space, (void*)0xfffffffff0000000, 0x10000000, MMUTable, PV_OFFSET);
#else
    rt_hw_mmu_map_init(&rt_kernel_space, (void*)0x080000000000ULL, 0x400000000ULL, MMUTable, 0);
#endif /* RT_USING_SMART */

    LOG_D("1rt_kernel_space [%p : %p]\n", rt_kernel_space.start, rt_kernel_space.size);
    rt_page_init(init_page_region);
    LOG_D("2rt_kernel_space [%p : %p]\n", rt_kernel_space.start, rt_kernel_space.size);
    rt_hw_mmu_setup(&rt_kernel_space, platform_mem_desc, platform_mem_desc_size);
    LOG_D("-->rt_hw_mmu_setup ok");
    /* map peripheral address to virtual address */
#ifdef RT_USING_HEAP
    /* initialize system heap */
    rt_system_heap_init((void *)HEAP_BEGIN, (void *)HEAP_END);
    LOG_D("-->rt_system_heap_init ok");
#endif

    /* initialize hardware interrupt */
    rt_hw_interrupt_init();
    LOG_D("-->rt_hw_interrupt_init ok");

    /* initialize uart */
    rt_hw_uart_init();
    LOG_D("-->rt_hw_uart_init ok");

    /* initialize timer for os tick */
    rt_hw_gtimer_init();

#ifdef RT_USING_CONSOLE
    /* set console device */
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
#endif /* RT_USING_CONSOLE */

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif
    rt_thread_idle_sethook(idle_wfi);

#ifdef RT_USING_SMP
    rt_smp_call_init();
    /* Install the IPI handle */
    rt_hw_ipi_handler_install(RT_SCHEDULE_IPI, rt_scheduler_ipi_handler);
    rt_hw_ipi_handler_install(RT_STOP_IPI, rt_scheduler_ipi_handler);
    rt_hw_ipi_handler_install(RT_SMP_CALL_IPI, rt_smp_call_ipi_handler);
    rt_hw_interrupt_umask(RT_SCHEDULE_IPI);
    rt_hw_interrupt_umask(RT_STOP_IPI);
    rt_hw_interrupt_umask(RT_SMP_CALL_IPI);
#endif
}

#ifdef RT_USING_SMP
#include <gic.h>

void rt_hw_mmu_ktbl_set(unsigned long tbl);
void _secondary_cpu_entry(void);

static unsigned long cpu_release_paddr[] =
{
    [0] = 0x401ff00,
    [1] = 0x401ff00,
    [2] = 0x401ff08,
    [3] = 0x401ff10,
    [4] = 0x401ff18,
    [5] = 0x401ff20,
    [6] = 0x401ff28,
    [7] = 0x401ff30,
};

void rt_hw_secondary_cpu_up(void)
{
    int i;
    void *release_addr;
    rt_uint64_t entry = (rt_uint64_t)rt_kmem_v2p(_secondary_cpu_entry);

    for (i = 1; i < RT_CPUS_NR && cpu_release_paddr[i]; ++i)
    {
#ifdef RT_USING_SMART
        release_addr = rt_ioremap((void *)cpu_release_paddr[i], sizeof(cpu_release_paddr[0]));
#else
        release_addr = (void *)cpu_release_paddr[i];
#endif
        __asm__ volatile ("str %0, [%1]"::"rZ"(entry), "r"(release_addr));
        LOG_D("release_addr[%d]: %p, 0x%llx, PV_OFFSET: 0x%llx", i, release_addr, *(unsigned long *)release_addr);
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, release_addr, sizeof(release_addr));
        rt_hw_barrier(dsb, sy);
        rt_hw_sev();
    }
#ifdef RT_USING_SMART
    rt_iounmap(release_addr);
#endif
}

void rt_hw_secondary_cpu_bsp_start(void)
{
    int cpu_id = rt_hw_cpu_id();

    rt_kprintf("\r\n", cpu_id);
    LOG_D("cpu %d start", cpu_id);

    system_vectors_init();
    LOG_D("system_vectors_init ok\n");

    rt_hw_spin_lock(&_cpus_lock);

    /* Save all mpidr */
    rt_hw_sysreg_read(mpidr_el1, rt_cpu_mpidr_table[cpu_id]);

    rt_hw_mmu_ktbl_set((unsigned long)MMUTable);

#ifdef RT_USING_PIC
    rt_pic_irq_init();
#else

    /* initialize vector table */
    rt_hw_vector_init();
    LOG_D("rt_hw_vector_init ok\n");

    // arm_gic_cpu_init(0, platform_get_gic_cpu_base());
    arm_gic_cpu_init(0, 0);
    LOG_D("arm_gic_cpu_init ok\n");
#ifdef BSP_USING_GICV3
    // arm_gic_redist_init(0, platform_get_gic_redist_base());
    arm_gic_redist_init(0, 0);
#endif /* BSP_USING_GICV3 */
#endif

#ifndef RT_CLOCK_TIME_ARM_ARCH
    /* initialize timer for os tick */
    rt_hw_gtimer_local_enable();
#endif /* !RT_CLOCK_TIME_ARM_ARCH */

    rt_hw_interrupt_umask(RT_SCHEDULE_IPI);
    rt_hw_interrupt_umask(RT_STOP_IPI);
    rt_hw_interrupt_umask(RT_SMP_CALL_IPI);

    LOG_I("Call cpu %d on %s", cpu_id, "success");

#if defined(RT_USING_CLOCK_TIME) && defined(RT_USING_DM)
    if (rt_clock_timer_us_delay == &cpu_us_delay)
    {
        cpu_loops_per_tick_init();
    }
#endif

    rt_system_scheduler_start();
}

void rt_hw_secondary_cpu_idle_exec(void)
{
    rt_hw_wfe();
}

#endif
