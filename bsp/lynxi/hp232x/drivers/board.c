/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 * 2021-12-28     GuEe-GUI       add smp support
 * 2025-06-17     lynxi          hp232x BSP, two-segment IRAM, direct boot
 */
#define DBG_TAG "board"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>
#include <smp_call.h>
#include <rthw.h>
#include <rtthread.h>
#include <mm_aspace.h>
#include <cpuport.h>

#include <rtconfig.h>
#include "board.h"
#include "drv_uart.h"
#include "hp232x_mmu.h"

#include "cp15.h"
#include <mmu.h>
#include <cpuport.h>
#include <interrupt.h>
#include <mm_page.h>
#include <gic.h>
#include <gicv3.h>
#include <gtimer.h>

/* Use RT-Thread standard MMU descriptor definitions from mmu.h
 * MMU_MAP_K_RWCB: Normal memory, cacheable, kernel RW
 * MMU_MAP_K_DEVICE: Device memory, kernel RW
 * These macros already include correct AF, SH, AP, MA bits
 */
#define MMU_TYPE_BLOCK 1UL
#define MMU_TYPE_TABLE 3UL

#define HP232X_CPU_STACK_BYTES 1024UL

extern size_t MMUTable[];
extern int __bss_start;
extern int __bss_end;
extern void *system_vectors;

static int is_uart_initialized = 0;

size_t gpio_base_addr = GPIO_BASE_ADDR;
size_t uart_base_addr = UART_BASE;
size_t gic_base_addr  = GIC_V2_BASE;
size_t arm_timer_base = ARM_TIMER_BASE;
size_t stimer_base_addr = STIMER_BASE;
size_t wdt_base_addr = WDT_BASE;

/* Simple UART direct output for early debug (before LOG system works) */
void early_putc_direct(char c)
{
    /* Direct UART output - works regardless of BSP_USING_HP232X_DEBUG_UART */
    /* This ensures boot progress is visible even when DEBUG_UART is disabled */
    volatile unsigned int *uart_thr = (volatile unsigned int *)0x10006000;
    volatile unsigned int *uart_lsr = (volatile unsigned int *)0x10006014;

    /* Wait for UART transmitter empty (TEMT bit in LSR) */
    while ((*uart_lsr & 0x40) == 0);

    /* Write character to THR */
    *uart_thr = (unsigned int)c;

    /* Add small delay to prevent UART overflow */
    for (int i = 0; i < 100; i++) { asm volatile("nop"); }
}

/*
 * HP232X memory descriptors for MMU setup (fixing address bounds for MPR/IRAM1):
 *   - IRAM0: kernel text/data/bss at 0x04000000 (512KB, NORMAL_MEM)
 *   - IRAM1: runtime data at 0x100040000 (high 256KB, up to 0x10007FFFF, NORMAL_MEM)
 *   - Peripherals at 0x08000000+ (DEVICE_MEM)
 *   - Boot ROM at 0x00000000 (NORMAL_MEM, 32MB bus window)
 *
 * Note: IRAM0 and IRAM1 are NOT contiguous — IRAM1 is at 4GB boundary.
 * Both use identity mapping (vaddr == paddr). IRAM1 mapping restricted to high 256KB.
 */
struct mem_desc platform_mem_desc[] = {
    /* IRAM0 — identity mapping at 0x04000000 (first 256KB usable) */
    {IRAM0_START, IRAM0_USE_END - 1, IRAM0_START, NORMAL_MEM},
    /* IRAM1 — identity mapping at 0x100040000 (last 256KB usable) */
    {IRAM1_USE_START, IRAM1_END - 1, IRAM1_USE_START, NORMAL_MEM},
    /* Peripheral space (GIC, UART, timer, etc.) */
    {INTC_BASE, INTC_BASE + 0x14000000UL - 1, INTC_BASE, DEVICE_MEM},
};

#ifdef BSP_USING_GICV3
rt_uint64_t rt_cpu_mpidr_table[] =
{
#if RT_CPUS_NR > 1
    [0] = 0x0UL,    /* CPU0 */
    [1] = 0x1UL,    /* CPU1 */
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

#ifdef RT_USING_SMP
static void system_vectors_init(void)
{
    rt_hw_set_current_vbar((rt_ubase_t)&system_vectors);
}
#endif

static rt_region_t init_page_region = {0, 0};

static rt_ubase_t hp232x_align_page(rt_ubase_t addr)
{
    return RT_ALIGN(addr, ARCH_PAGE_SIZE);
}

/**
 *  Initialize the Hardware related stuffs. Called from rtthread_startup()
 *  after interrupt disabled.
 *
 *  On HP232X, the chip boots directly from bootcode (no bootwrapper/SPL).
 *  The bootcode already set up EL3 → EL1 transition, GIC, and interconnect
 *  before jumping to rt-thread entry.  We only need to configure MMU,
 *  interrupts, UART, and timer here.
 */
void rt_hw_board_init(void)
{
    /* ===== EARLY DIAGNOSTIC OUTPUT ===== */
    /* Print boot marker BEFORE any initialization - works without DEBUG_UART */
    early_putc_direct('\n');
    early_putc_direct('B');
    early_putc_direct('O');
    early_putc_direct('O');
    early_putc_direct('T');
    early_putc_direct('\n');

    rt_ubase_t bss_end;
    rt_ubase_t noclean_end;
    rt_ubase_t bss_start;
    rt_ubase_t page_start;
    rt_ubase_t page_end;
    rt_ubase_t heap_start;
    rt_ubase_t heap_end;
    rt_ubase_t stack_guard_base;

    noclean_end = (rt_ubase_t)&__bss_start;
    bss_start = noclean_end;
    bss_end = (rt_ubase_t)&__bss_end;
    page_start = hp232x_align_page(bss_end);
    page_end = page_start + PAGE_POOL_SIZE;
    heap_start = page_end;
    heap_end = heap_start + HEAP_POOL_SIZE;
    stack_guard_base = IRAM1_STACK_TOP
        - HP232X_CPU_STACK_BYTES * (RT_CPUS_NR - 1)
        - HP232X_CPU_STACK_BYTES;

    /* HP232X places .bss in IRAM1 above 4GB. entry_point.S skips early
     * zeroing for this high address, so clear it here before any BSS globals
     * such as interrupt hooks, earlycon_base, and scheduler state are used. */
    rt_memset((void *)bss_start, 0, bss_end - bss_start);

    rt_hw_earlycon_ioremap_early();

    RT_ASSERT(noclean_end >= IRAM1_USE_START);
    RT_ASSERT(bss_start >= IRAM1_USE_START);
    RT_ASSERT(heap_end <= IRAM1_END);
    RT_ASSERT(heap_end <= stack_guard_base);

    init_page_region.start = page_start;
    init_page_region.end = page_end;

    rt_kprintf("IRAM1: bss@%lx-%lx page@%lx-%lx heap@%lx-%lx",
          bss_start, bss_end, page_start, page_end, heap_start, heap_end);

    LOG_I("[board] STEP1: HP232X MMU initialization");
    hp232x_mmu_init();

    LOG_I("[board] MMU initialization complete - proceeding to heap setup");

#ifdef RT_USING_HEAP
    rt_system_heap_init((void *)heap_start, (void *)heap_end);
#endif

    LOG_I("[board] starting GICv3 init...");

    /* Initialize GIC */
    rt_hw_interrupt_init();

    /* NOTE: GIC Secure configuration (ARE_NS, UART Group1) is done in pre_entry.S at EL3 */
    /* EL1 Non-Secure cannot modify GIC Secure registers - these attempts will fail */

    /* Verify GIC configuration (read-only at EL1 NS) */
    rt_kprintf("\n[DEBUG] ===== Verifying GIC configuration =====\n");

    volatile uint32_t *gicd_ctrl = (volatile uint32_t *)(GIC_PL500_DISTRIBUTOR_PPTR + 0x000);
    uint32_t gicd_ctlr = *gicd_ctrl;
    rt_kprintf("  GICD_CTLR: 0x%x (ARE_NS view at EL1 NS)\n", gicd_ctlr);

    volatile uint32_t *gicr_isenabler0 = (volatile uint32_t *)(0x08100000 + 0x1100);  // GICR_ISENABLER0
    volatile uint8_t *gicr_ipriorityr = (volatile uint8_t *)(0x08100000 + 0x4100);    // GICR_IPRIORITYR base

    LOG_I("  Timer IRQ30 enabled: bit30=%d (expect 1)",
          (*gicr_isenabler0 >> 30) & 1);
    LOG_I("  Timer IRQ30 priority: 0x%02x (expect 0xa0)",
          gicr_ipriorityr[30]);

    rt_kprintf("========================================\n\n");

    /* Check ICC_IGRPEN1_EL1 (Interrupt Group Enable) */
    uint64_t igprpen1;
    __asm__ volatile("mrs %0, S3_0_C12_C12_7" : "=r"(igprpen1));  // ICC_IGRPEN1_EL1
    LOG_I("  ICC_IGRPEN1_EL1 = 0x%llx (Enable=%d)", igprpen1, (igprpen1 >> 0) & 1);

    LOG_I("[board] ===== GICv3 Initialization Complete =====");

    /* ===== Timer interrupt test ===== */
    LOG_I("[board] Testing Timer interrupt...");

    /* Check CNTP timer configuration */
    uint64_t cntp_ctl, cntp_cval, cntp_tval;
    __asm__ volatile("mrs %0, CNTP_CTL_EL0" : "=r"(cntp_ctl));
    __asm__ volatile("mrs %0, CNTP_CVAL_EL0" : "=r"(cntp_cval));
    __asm__ volatile("mrs %0, CNTP_TVAL_EL0" : "=r"(cntp_tval));

    LOG_I("  CNTP_CTL_EL0  = 0x%llx (ENABLE=%d, IMASK=%d, ISTATUS=%d)",
          cntp_ctl, (cntp_ctl >> 0) & 1, (cntp_ctl >> 1) & 1, (cntp_ctl >> 2) & 1);
    LOG_I("  CNTP_CVAL_EL0 = 0x%llx", cntp_cval);
    LOG_I("  CNTP_TVAL_EL0 = 0x%llx", cntp_tval);

    /* Manually enable timer interrupt if needed */
    if (((*gicr_isenabler0 >> 30) & 1) == 0) {
        LOG_W("[board] Timer IRQ30 not enabled! Manually enabling...");
        *gicr_isenabler0 = (1 << 30);  /* Enable IRQ 30 */
        gicr_ipriorityr[30] = 0xa0;    /* Set priority */
        __DSB();
        LOG_I("  Timer IRQ30 manually enabled");
    }

    /* Test timer interrupt trigger */
    if ((cntp_ctl & 1) == 0) {
        LOG_W("[board] CNTP timer not enabled! Testing timer...");

        /* Set a short timer value for test */
        uint64_t cntpct;
        __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(cntpct));
        cntp_cval = cntpct + 10000;  /* Trigger after 10000 cycles */
        __asm__ volatile("msr CNTP_CVAL_EL0, %0" :: "r"(cntp_cval));

        cntp_ctl = 1;  /* ENABLE=1, IMASK=0 */
        __asm__ volatile("msr CNTP_CTL_EL0, %0" :: "r"(cntp_ctl));
        __ISB();

        LOG_I("  Timer test: CNTPCT=%llx, CVAL=%llx, CTL=%llx", cntpct, cntp_cval, cntp_ctl);
        LOG_I("  Waiting for timer interrupt...");

        /* Wait a bit to see if interrupt triggers */
        for (int i = 0; i < 100000; i++) { __asm__ volatile("nop"); }

        /* Check if interrupt triggered */
        __asm__ volatile("mrs %0, CNTP_CTL_EL0" : "=r"(cntp_ctl));
        LOG_I("  After wait: CNTP_CTL_EL0 = 0x%llx (ISTATUS=%d)",
              cntp_ctl, (cntp_ctl >> 2) & 1);
    }

    /* initialize uart */
    rt_hw_uart_init();
    is_uart_initialized = 1;
    LOG_I("-->rt_hw_uart_init ok\n");

    /* initialize timer for os tick */
    rt_hw_gtimer_init();
    LOG_I("-->rt_hw_gtimer_init ok\n");

#ifdef RT_USING_CONSOLE
    /* set console device */
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
    LOG_I("-->rt_console_set_device ok\n");
#endif /* RT_USING_CONSOLE */

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif

#ifdef RT_USING_SMP
    LOG_I("[board] SKIP SMP initialization (GIC not initialized yet)");
    // rt_smp_call_init();
    // /* Install the IPI handle */
    // rt_hw_ipi_handler_install(RT_SCHEDULE_IPI, rt_scheduler_ipi_handler);
    // rt_hw_ipi_handler_install(RT_STOP_IPI, rt_scheduler_ipi_handler);
    // rt_hw_ipi_handler_install(RT_SMP_CALL_IPI, rt_smp_call_ipi_handler);
    // rt_hw_interrupt_umask(RT_SCHEDULE_IPI);
    // rt_hw_interrupt_umask(RT_STOP_IPI);
    // rt_hw_interrupt_umask(RT_SMP_CALL_IPI);
#endif

    LOG_I("[board] ===== BOARD INIT COMPLETE =====");
    LOG_I("[board] Next: rt_show_version() → rt_application_init()");
}

#ifdef RT_USING_SMP
#include <gic.h>

void rt_hw_mmu_ktbl_set(unsigned long tbl);
void _secondary_cpu_entry(void);

/*
 * Mailbox addresses for secondary CPUs.
 *
 * Current scheme:
 * - primary CPU writes physical entry address to mailbox
 * - secondary CPU is released by SEV and enters _secondary_cpu_entry
 *
 * This is compatible with the bootwrapper spin-table idea at a high level,
 * but not yet the same as an early assembly-side WFE/WFI polling loop.
 */
static unsigned long cpu_release_paddr[] =
{
    [0] = MBOX_ADDRESS,       /* CPU0 (primary, unused) */
    [1] = MBOX_ADDRESS + 0x8, /* CPU1 mailbox */
};

void rt_hw_secondary_cpu_up(void)
{
    int i;
    void *release_addr;
    rt_uint64_t entry = (rt_uint64_t)rt_kmem_v2p(_secondary_cpu_entry);

    for (i = 0; i < RT_CPUS_NR && cpu_release_paddr[i]; ++i)
    {
        release_addr = (void *)cpu_release_paddr[i];
        __asm__ volatile ("str xzr, [%0]"::"r"(release_addr):"memory");
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, release_addr, sizeof(rt_uint64_t));
    }

    rt_hw_barrier(dsb, sy);

    for (i = 1; i < RT_CPUS_NR && cpu_release_paddr[i]; ++i)
    {
        release_addr = (void *)cpu_release_paddr[i];
        __asm__ volatile ("str %0, [%1]"::"rZ"(entry), "r"(release_addr):"memory");
        LOG_D("release_addr[%d]: %p, 0x%llx", i, release_addr, *(unsigned long *)release_addr);
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, release_addr, sizeof(rt_uint64_t));
        rt_hw_barrier(dsb, sy);
        rt_hw_sev();
    }
}

void rt_hw_secondary_cpu_bsp_start(void)
{
    int cpu_id = rt_hw_cpu_id();

    LOG_D("cpu %d start", cpu_id);

    system_vectors_init();
    LOG_D("system_vectors_init ok\n");

    rt_hw_spin_lock(&_cpus_lock);

    /* Save all mpidr */
    rt_hw_sysreg_read(mpidr_el1, rt_cpu_mpidr_table[cpu_id]);

    rt_hw_mmu_ktbl_set((unsigned long)MMUTable);

#ifndef RT_CLOCK_TIME_ARM_ARCH
    /* initialize timer for os tick */
    rt_hw_gtimer_local_enable();
#endif /* !RT_CLOCK_TIME_ARM_ARCH */

    rt_hw_interrupt_umask(RT_SCHEDULE_IPI);
    rt_hw_interrupt_umask(RT_STOP_IPI);
    rt_hw_interrupt_umask(RT_SMP_CALL_IPI);

    LOG_I("Call cpu %d on %s", cpu_id, "success");

    rt_system_scheduler_start();
}

void rt_hw_secondary_cpu_idle_exec(void)
{
    rt_hw_wfe();
}

#endif
