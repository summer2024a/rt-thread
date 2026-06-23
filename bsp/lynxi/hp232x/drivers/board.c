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

size_t gpio_base_addr = GPIO_BASE_ADDR;
size_t uart_base_addr = UART_BASE;
size_t gic_base_addr  = GIC_V2_BASE;
size_t arm_timer_base = ARM_TIMER_BASE;
size_t stimer_base_addr = STIMER_BASE;
size_t wdt_base_addr = WDT_BASE;

/* Simple UART direct output for early debug (before LOG system works) */
#ifdef BSP_USING_HP232X_DEBUG_UART
void early_putc_direct(char c)
{
    volatile unsigned int *uart_thr = (volatile unsigned int *)0x10006000;
    volatile unsigned int *uart_lsr = (volatile unsigned int *)0x10006014;

    /* Wait for UART transmitter empty (TEMT bit in LSR) */
    while ((*uart_lsr & 0x40) == 0);

    /* Write character to THR */
    *uart_thr = (unsigned int)c;

    /* Add small delay to prevent UART overflow */
    for (int i = 0; i < 1000; i++) { asm volatile("nop"); }
}
#else
void early_putc_direct(char c) { (void)c; }  /* Empty stub when DEBUG_UART disabled */
#endif

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

static void system_vectors_init(void)
{
    rt_hw_set_current_vbar((rt_ubase_t)&system_vectors);
}

extern size_t MMUTable[];

/* ===== HP232X专用MMU页表（放置在IRAM0） ===== */
/*
 * CRITICAL: 页表必须放在IRAM0的.mmu_table section
 * 原因：IRAM0地址<4GB，MMU启用前后都可安全访问
 * 虽然ARMv8-A在无MMU时可访问IRAM1（40-bit PA），但IRAM0更安全
 */
#define HP232X_PGD_SIZE   512   /* Level 0: 512 entries covering 512GB each */
#define HP232X_PUD_SIZE   512   /* Level 1: 512 entries covering 1GB each */
#define HP232X_PMD_SIZE   512   /* Level 2: 512 entries covering 2MB each */

/* Level 0 page table (PGD) - in IRAM0 .mmu_table section */
rt_size_t hp232x_pgd[HP232X_PGD_SIZE] __attribute__((section(".mmu_table"), aligned(4096)));

/* Level 1 page table (PUD) for region 0 (0-512GB) - in IRAM0 */
static rt_size_t pud_table_0[HP232X_PUD_SIZE] __attribute__((section(".mmu_table"), aligned(4096)));

/* Level 2 page table (PMD) for fine-grained mapping of 0GB-1GB region
 * Each entry covers 2MB, total 512 entries = 1GB
 * This allows mixing Normal Memory (IRAM0) and Device Memory (GIC/UART)
 */
static rt_size_t pmd_table_0[HP232X_PMD_SIZE] __attribute__((section(".mmu_table"), aligned(4096)));

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
    rt_ubase_t bss_end;
    rt_ubase_t noclean_end;
    rt_ubase_t bss_start;
    rt_ubase_t page_start;
    rt_ubase_t page_end;
    rt_ubase_t heap_start;
    rt_ubase_t heap_end;
    rt_ubase_t stack_guard_base;

    rt_hw_earlycon_ioremap_early();

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

    RT_ASSERT(noclean_end >= IRAM1_USE_START);
    RT_ASSERT(bss_start >= IRAM1_USE_START);
    RT_ASSERT(heap_end <= IRAM1_END);
    RT_ASSERT(heap_end <= stack_guard_base);

    init_page_region.start = page_start;
    init_page_region.end = page_end;

    LOG_I("IRAM1: bss@%lx-%lx page@%lx-%lx heap@%lx-%lx",
          bss_start, bss_end, page_start, page_end, heap_start, heap_end);

    /* ===== MMU配置 - 简化方案 ===== */
    LOG_I("[board] STEP1: Manual MMU setup (3-level page tables in IRAM0)");
    /*
     * HP232X MMU配置方案（简化版）
     *
     * Identity mapping策略：
     *   IRAM0 (0x04000000, 64MB): PMD[32-63], NORMAL_MEM
     *   GIC  (0x08000000, 128MB): PMD[64-127], DEVICE_MEM
     *   UART (0x10006000, 256MB区域): PMD[128-255], DEVICE_MEM
     *   IRAM1 (0x100000000, 4GB): PUD[4] 1GB block, NORMAL_MEM
     *
     * 页表存储：所有页表在IRAM0 .mmu_table section
     */

    rt_size_t *pgd = hp232x_pgd;
    rt_memset(pgd, 0, sizeof(hp232x_pgd));
    rt_memset(pud_table_0, 0, sizeof(pud_table_0));
    rt_memset(pmd_table_0, 0, sizeof(pmd_table_0));

    early_putc_direct('M');  /* Mark: MMU init start */

    /* Step 1: PGD → PUD link */
    pgd[0] = ((rt_size_t)pud_table_0 & ~0x3FFUL) | MMU_TYPE_TABLE;  /* VA 0-512GB */
    LOG_D("pgd[0]=0x%lx → pud_table_0 @ 0x%lx", pgd[0], (rt_size_t)pud_table_0);

    /* Step 2: PUD[0] → PMD link (VA 0-1GB, mixed attributes) */
    pud_table_0[0] = ((rt_size_t)pmd_table_0 & ~0x3FFUL) | MMU_TYPE_TABLE;
    LOG_D("pud_table_0[0]=0x%lx → pmd_table_0 @ 0x%lx", pud_table_0[0], (rt_size_t)pmd_table_0);

    /* Step 3: PMD entries for VA 0-1GB region
     * Each PMD entry covers 2MB block
     * PMD index = phys_addr / 2MB
     *
     * Descriptor format:
     *   descriptor = (pmd_index << 21) | attrs | MMU_TYPE_BLOCK
     */
    early_putc_direct('P');  /* Mark: PMD setup */

    /* PMD[0-31]: VA 0-64MB, DEVICE (unused) */
    for (int i = 0; i < 32; i++) {
        pmd_table_0[i] = ((rt_size_t)i << 21) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;
    }

    /* PMD[32-63]: VA 64-128MB (IRAM0 @ 0x04000000), NORMAL_MEM */
    for (int i = 32; i < 64; i++) {
        pmd_table_0[i] = ((rt_size_t)i << 21) | MMU_MAP_K_RWCB | MMU_TYPE_BLOCK;
    }
    LOG_D("pmd[32]=0x%lx (IRAM0 start)", pmd_table_0[32]);

    /* PMD[64-127]: VA 128-256MB (GIC @ 0x08000000), DEVICE_MEM */
    for (int i = 64; i < 128; i++) {
        pmd_table_0[i] = ((rt_size_t)i << 21) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;
    }
    LOG_D("pmd[64]=0x%lx (GIC start)", pmd_table_0[64]);

    /* PMD[128-255]: VA 256-512MB (Peripherals including UART @ 0x10006000), DEVICE_MEM */
    for (int i = 128; i < 256; i++) {
        pmd_table_0[i] = ((rt_size_t)i << 21) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;
    }
    LOG_D("pmd[128]=0x%lx (UART area)", pmd_table_0[128]);

    /* PMD[256-511]: VA 512MB-1GB, DEVICE (unused) */
    for (int i = 256; i < 512; i++) {
        pmd_table_0[i] = ((rt_size_t)i << 21) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;
    }

    early_putc_direct('D');  /* Mark: PMD done */

    /* Step 4: PUD[1-3]: VA 1-4GB, DEVICE (unused) */
    for (int i = 1; i < 4; i++) {
        pud_table_0[i] = ((rt_size_t)i << 30) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;
    }

    /* Step 5: PUD[4]: VA 4-5GB (IRAM1 @ 0x100000000), NORMAL_MEM
     * CRITICAL: IRAM1映射，使用1GB block descriptor
     * Descriptor = (GB_index << 30) | attrs | MMU_TYPE_BLOCK
     * GB_index = 4 (对应4GB-5GB范围)
     */
    pud_table_0[4] = (4ULL << 30) | MMU_MAP_K_RWCB | MMU_TYPE_BLOCK;
    LOG_I("pud_table_0[4]=0x%llx (IRAM1 @ 4GB)", pud_table_0[4]);

    /* PUD[5-511]: VA 5-512GB, DEVICE (unused) */
    for (int i = 5; i < 512; i++) {
        pud_table_0[i] = ((rt_size_t)i << 30) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;
    }

    early_putc_direct('U');  /* Mark: PUD done */

    /* Verify key descriptors */
    LOG_I("Page table setup complete:");
    LOG_I("  pgd[0]=0x%lx (links to pud)", pgd[0]);
    LOG_I("  pud[0]=0x%lx (links to pmd)", pud_table_0[0]);
    LOG_I("  pud[4]=0x%llx (IRAM1 block)", pud_table_0[4]);
    LOG_I("  pmd[32]=0x%lx pmd[64]=0x%lx pmd[128]=0x%lx",
          pmd_table_0[32], pmd_table_0[64], pmd_table_0[128]);

/* Step 6: Configure MAIR_EL1 (Memory Attribute Indirection Register) */
    LOG_I("[board] STEP2: Configure MMU registers");
    early_putc_direct('A');  /* Mark: Before MAIR */

    unsigned long mair = 0x00447fUL;  /* RT-Thread standard value */
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Verify MAIR_EL1 */
    unsigned long mair_verify;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair_verify));
    LOG_I("MAIR_EL1: written=0x%lx read=0x%lx", mair, mair_verify);

    /* Step 7: Configure TCR_EL1 (Translation Control Register)
     * IPS=2 (40-bit PA, supports IRAM1 @ 4GB)
     * TG0=0 (4KB granule), SH0=3 (Inner Shareable)
     * ORGN0=1, IRGN0=1 (Cacheable)
     */
    unsigned long tcr = (2UL << 32)    /* IPS=2: 40-bit PA */
                      | (0UL << 14)    /* TG0=0: 4KB granule */
                      | (3UL << 12)    /* SH0=3: Inner shareable */
                      | (1UL << 10)    /* ORGN0=1: Outer Cacheable */
                      | (1UL << 8)     /* IRGN0=1: Inner Cacheable */
                      | (0UL << 0);    /* T0SZ=0: 64-bit VA */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("isb" ::: "memory");
    LOG_I("TCR_EL1 configured: 0x%lx", tcr);

    early_putc_direct('T');  /* Mark: After TCR */

    /* Step 8: Set TTBR0_EL1 (Translation Table Base Register) */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((unsigned long)pgd) : "memory");
    __asm__ volatile("isb" ::: "memory");
    LOG_I("TTBR0_EL1 set to pgd @ 0x%lx", (unsigned long)pgd);

    /* Step 9: Data Synchronization Barrier */
    __asm__ volatile("dsb sy" ::: "memory");
    early_putc_direct('B');  /* Mark: Before MMU enable */

    /* Step 10: Enable MMU (Phase 1: MMU only, no caches) */
    LOG_I("[board] STEP3: Enable MMU");
    unsigned long sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    LOG_I("SCTLR_EL1 before: 0x%lx (M=%d C=%d I=%d)",
          sctlr, (sctlr & 1) ? 1 : 0, (sctlr & 4) ? 1 : 0, (sctlr & 0x1000) ? 1 : 0);

    sctlr |= 0x1UL;      /* Enable MMU */
    sctlr &= ~0x4UL;     /* Disable data cache (for safety) */
    sctlr &= ~0x1000UL;  /* Disable instruction cache */

    LOG_I("Enabling MMU: SCTLR=0x%lx", sctlr);
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    early_putc_direct('1');  /* Mark: MMU enabled */
    LOG_I("MMU enabled successfully!");

    /* Step 11: Verify MMU and test IRAM1 access */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    LOG_I("SCTLR after MMU enable: 0x%lx (M=%d C=%d I=%d)",
          sctlr, (sctlr & 1) ? 1 : 0, (sctlr & 4) ? 1 : 0, (sctlr & 0x1000) ? 1 : 0);

    /* Test IRAM1 access through MMU */
    volatile unsigned long *iram1_test = (volatile unsigned long *)0x100040000;
    unsigned long iram1_val = *iram1_test;
    LOG_I("IRAM1 test: read 0x%lx from 0x100040000", iram1_val);
    early_putc_direct('R');  /* Mark: IRAM1 read OK */

    /* Step 12: Enable caches (Phase 2) */
    LOG_I("[board] STEP4: Enable caches");
    sctlr |= 0x4UL;     /* Enable data cache */
    sctlr |= 0x1000UL;  /* Enable instruction cache */

    LOG_I("Enabling caches: SCTLR=0x%lx", sctlr);
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    early_putc_direct('C');  /* Mark: Caches enabled */
    LOG_I("MMU + caches fully enabled!");

    /* Final verification */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    LOG_I("Final SCTLR_EL1: 0x%lx (M=%d C=%d I=%d)",
          sctlr, (sctlr & 1) ? 1 : 0, (sctlr & 4) ? 1 : 0, (sctlr & 0x1000) ? 1 : 0);

    LOG_I("[board] MMU initialization complete - proceeding to heap setup");

#ifdef RT_USING_HEAP
    rt_system_heap_init((void *)heap_start, (void *)heap_end);
    LOG_I("[board] Heap: 0x%lx-0x%lx (%ldKB)",
          heap_start, heap_end, (heap_end - heap_start) / 1024);
#endif

    /* initialize hardware interrupt */
    LOG_I("[board] About to call rt_interrupt_init (GICv3)");
    early_putc_direct('G');  /* Mark: Before GIC init */
    rt_hw_interrupt_init();
    early_putc_direct('g');  /* Mark: After GIC init */
    LOG_I("[board] rt_interrupt_init OK");

    /* initialize uart */
    early_putc_direct('U');  /* Mark: Before UART init */
    rt_hw_uart_init();
    early_putc_direct('u');  /* Mark: After UART init */
    LOG_D("-->rt_hw_uart_init ok");

    /* initialize timer for os tick */
    early_putc_direct('T');  /* Mark: Before Timer init */
    rt_hw_gtimer_init();
    early_putc_direct('t');  /* Mark: After Timer init */

#ifdef RT_USING_CONSOLE
    /* set console device */
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
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
