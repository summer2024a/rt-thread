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

#include <rthw.h>
#include <rtthread.h>
#include <mm_aspace.h>

#include "board.h"
#include "drv_uart.h"

#include "cp15.h"
#include "mmu.h"
#include <mm_page.h>

#ifdef RT_USING_SMART
#include <lwp_arch.h>
#endif

#define MEM_DESC(vaddr_start, size, paddr_start, attr) \
    vaddr_start, (vaddr_start + size - 1uL), paddr_start, attr

extern size_t MMUTable[];

size_t gpio_base_addr = GPIO_BASE_ADDR;

size_t uart_base_addr = UART_BASE;

size_t gic_base_addr = GIC_V2_BASE;

size_t arm_timer_base = ARM_TIMER_BASE;

size_t stimer_base_addr = STIMER_BASE;

size_t mmc2_base_addr   = MMC2_BASE_ADDR;

size_t wdt_base_addr = WDT_BASE;

uint8_t *mac_reg_base_addr = (uint8_t *)MAC_REG;

uint8_t *eth_send_no_cache = (uint8_t *)SEND_DATA_NO_CACHE;
uint8_t *eth_recv_no_cache = (uint8_t *)RECV_DATA_NO_CACHE;

#ifdef RT_USING_SMART
struct mem_desc platform_mem_desc[] = {
    {KERNEL_VADDR_START, KERNEL_VADDR_START + 0x0fffffff, (rt_size_t)ARCH_MAP_FAILED, NORMAL_MEM}
};
#else
// struct mem_desc platform_mem_desc[] = {
//     {MEM_PADDR_START, MEM_PADDR_START + MEM_CACHE_SZ - 1, MEM_PADDR_START, NORMAL_MEM},
//     {MEM_PADDR_START+MEM_CACHE_SZ, MEM_PADDR_START+MEM_CACHE_SZ+MEM_NOCACHE_SZ - 1, MEM_PADDR_START+MEM_CACHE_SZ, NORMAL_MEM},
//     {MEM_PADDR_START+MEM_CACHE_SZ+MEM_NOCACHE_SZ, MEM_PADDR_START+MEM_CACHE_SZ+MEM_NOCACHE_SZ+0x1BFFFFFF, 0x0, DEVICE_MEM},
// };

struct mem_desc platform_mem_desc[] = {
    {INTC_BASE, INTC_BASE + 0x200000 - 1, INTC_BASE, DEVICE_MEM},
    {0x10006000, 0x10007FFF, 0x10006000, DEVICE_MEM}, /* uart0,1 */
    {0x000800000000ULL, 0x00082FFFFFFFULL, 0x000800000000ULL, NORMAL_MEM},
};

#endif

#ifdef BSP_USING_GICV3
rt_uint64_t rt_cpu_mpidr_table[] =
{
    [RT_CPUS_NR] = 0,
};
#endif

const rt_uint32_t platform_mem_desc_size = sizeof(platform_mem_desc)/sizeof(platform_mem_desc[0]);

void idle_wfi(void)
{
    asm volatile ("wfi");
}

/**
 * This function will initialize board
 */

extern size_t MMUTable[];
int rt_hw_gtimer_init(void);

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
    rt_hw_mmu_map_init(&rt_kernel_space, (void*)0x080000000000ULL, 0x40000000ULL, MMUTable, 0);
#endif /* RT_USING_SMART */

    LOG_D("1rt_kernel_space [%p : %p]\n", rt_kernel_space.start, rt_kernel_space.size);
    rt_page_init(init_page_region);
    LOG_D("2rt_kernel_space [%p : %p]\n", rt_kernel_space.start, rt_kernel_space.size);
    rt_hw_mmu_setup(&rt_kernel_space, platform_mem_desc, platform_mem_desc_size);
    // rt_hw_ioremap_after_mmu();
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
}

#ifdef RT_USING_SMP
#include <gic.h>

void rt_hw_mmu_ktbl_set(unsigned long tbl);
void _secondary_cpu_entry(void);

static unsigned long cpu_release_paddr[] =
{
    [0] = 0xd8,
    [1] = 0xe0,
    [2] = 0xe8,
    [3] = 0xf0,
    [4] = 0x00
};

void rt_hw_secondary_cpu_up(void)
{
    int i;
    void *release_addr;

    for (i = 1; i < RT_CPUS_NR && cpu_release_paddr[i]; ++i)
    {
        release_addr = rt_ioremap((void *)cpu_release_paddr[i], sizeof(cpu_release_paddr[0]));
        __asm__ volatile ("str %0, [%1]"::"rZ"((unsigned long)_secondary_cpu_entry + PV_OFFSET), "r"(release_addr));
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, release_addr, sizeof(release_addr));
        asm volatile ("dsb sy");
        asm volatile ("sev");
    }
}

void rt_hw_secondary_cpu_bsp_start(void)
{
    rt_hw_spin_lock(&_cpus_lock);

    rt_hw_mmu_ktbl_set((unsigned long)MMUTable);

    rt_hw_vector_init();

    arm_gic_cpu_init(0, 0);

    rt_hw_gtimer_init();

    rt_kprintf("\rcpu %d boot success\n", rt_hw_cpu_id());

    rt_system_scheduler_start();
}

void rt_hw_secondary_cpu_idle_exec(void)
{
    asm volatile ("wfe":::"memory", "cc");
}

#endif
