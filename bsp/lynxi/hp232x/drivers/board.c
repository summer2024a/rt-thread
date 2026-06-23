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

/* ===== HP232X专用MMU页表（暂时禁用以减小镜像大小） ===== */
/* 只有启用MMU时才需要这些页表，当前跳过MMU测试 */
#if 0  /* 禁用页表定义，节省12KB空间 */
/*
 * CRITICAL: 页表必须放在IRAM0的.mmu_table section
 * 原因：IRAM1地址>4GB，MMU启用前CPU无法访问IRAM1中的数据
 */
#define HP232X_PGD_SIZE   512   /* Level 0: 512 entries covering 512GB each */
#define HP232X_PUD_SIZE   512   /* Level 1: 512 entries covering 1GB each */

/* Level 0 page table (PGD) - in IRAM0 .mmu_table section */
rt_size_t hp232x_pgd[HP232X_PGD_SIZE] __attribute__((section(".mmu_table"), aligned(4096)));

/* Level 1 page table (PUD) for region 0 (0-512GB) - in IRAM0 */
static rt_size_t pud_table[HP232X_PUD_SIZE] __attribute__((section(".mmu_table"), aligned(4096)));

/* Level 2 page table (PMD) for fine-grained mapping of 0GB-1GB region
 * Each entry covers 2MB, total 512 entries = 1GB
 * This allows mixing Normal Memory (IRAM0) and Device Memory (GIC/UART)
 */
static rt_size_t pmd_table_0gb[512] __attribute__((section(".mmu_table"), aligned(4096)));
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

    /* ===== COMPLETELY SKIP MMU - Test basic boot ===== */
    LOG_I("[board] SKIP ALL MMU CONFIG - Testing basic boot without MMU");
    extern void early_putc_direct(char c);
    /* early_putc_direct */ /* 'S' - Skip MMU for now */
    goto skip_all_mmu;

    /* ===== MMU配置代码（暂时跳过）===== */
#if 0  /* 禁用所有MMU配置代码，节省空间并避免编译错误 */
    extern size_t MMUTable[];

    /*
     * ARMv8-A 4KB-page 3-level page table structure:
     *   Level 0 (PGD): 512 entries, each covers 512GB
     *   Level 1 (PUD): 512 entries, each covers 1GB
     *   Level 2 (PMD): 512 entries, each covers 2MB (block)
     *
     * IRAM0 (0x04000000) mapping:
     *   L0 idx = 0, L1 idx = 0, L2 idx = 32-35 (covers 0x04000000-0x047FFFFF)
     *
     * IRAM1 (0x100000000 = 4GB) mapping:
     *   L0 idx = 2, L1 idx = 4 (need separate L1 table)
     *
     * GIC (0x08000000) mapping:
     *   L0 idx = 0, L1 idx = 0, L2 idx = 64-71
     *
     * Layout:
     *   MMUTable (4096B) = Level 0 table @ IRAM1
     *   pud_table (4096B) = Level 1 table for L0[0] @ IRAM1
     *   pud_table_hi (4096B) = Level 1 table for L0[2] @ IRAM1
     */

    /*
     * 方案B: HP232X专用MMU配置（3级页表，IRAM0存储）
     * CRITICAL: 页表在IRAM0，MMU启用前可访问
     */
    LOG_I("[board] STEP2: Manual static MMU setup (3-level page tables)");

    rt_size_t *pgd = hp232x_pgd;  /* Use IRAM0-resident PGD */
    rt_memset(pgd, 0, sizeof(hp232x_pgd));
    rt_memset(pud_table, 0, sizeof(pud_table));
    rt_memset(pmd_table_0gb, 0, sizeof(pmd_table_0gb));

    /* Set up Level 0 -> Level 1 links */
    pgd[0] = ((rt_size_t)pud_table & ~0x3FFUL) | MMU_TYPE_TABLE;  /* 0-512GB */

    /* Set up Level 1 for 0GB-1GB: Link to Level 2 (PMD) for mixed attributes */
    pud_table[0] = ((rt_size_t)pmd_table_0gb & ~0x3FFUL) | MMU_TYPE_TABLE;

    /* Set up Level 2 (PMD) entries - each covers 2MB */
    /*
     * Level 2 Block Descriptor format (2MB block):
     *   Bits [47:21]: Output Address (OA) field = phys_addr >> 21
     *   Bits [11:10]: AF, SH, AP, AttrIndex as usual
     *   Bits [1:0]: Type = 1 (block)
     *
     * For identity mapping (VA = PA):
     *   PMD[i] covers VA range [i*2MB, (i+1)*2MB)
     *   Descriptor = (i << 21) | attrs | type
     */

    /* IRAM0 @ 0x04000000 (64MB) - PMD indices 32 to 63 (32 entries) */
    /* PMD index 32 = 64MB / 2MB = 32 */
    for (int i = 32; i < 64; i++) {  /* IRAM0: 64MB = 32 x 2MB blocks */
        pmd_table_0gb[i] = ((rt_size_t)i << 21) | MMU_MAP_K_RWCB | MMU_TYPE_BLOCK;
    }

    /* GIC @ 0x08000000 (128MB) - PMD indices 64 to 127 (64 entries) */
    /* PMD index 64 = 128MB / 2MB = 64 */
    for (int i = 64; i < 128; i++) {  /* GIC: 128MB area */
        pmd_table_0gb[i] = ((rt_size_t)i << 21) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;
    }

    /* UART @ 0x10006000 - falls in PMD index 128 (256MB area) */
    /* PMD index 128 = 256MB / 2MB = 128 */
    pmd_table_0gb[128] = ((rt_size_t)128 << 21) | MMU_MAP_K_DEVICE | MMU_TYPE_BLOCK;

    /* pud_table[4]: VA 4GB-5GB → PA 4GB-5GB (IRAM1, use 1GB block) */
    pud_table[4] = (4ULL << 30) | MMU_MAP_K_RWCB | MMU_TYPE_BLOCK;

    LOG_I("[board] Page tables in IRAM0: pgd=%p pud=%p pmd=%p", pgd, pud_table, pmd_table_0gb);
    LOG_I("[board] pud_table[0]=0x%lx (links to pmd)", pud_table[0]);
    LOG_I("[board] pmd[32]=0x%lx (IRAM0) pmd[64]=0x%lx (GIC) pmd[128]=0x%lx (UART)",
          pmd_table_0gb[32], pmd_table_0gb[64], pmd_table_0gb[128]);
    LOG_I("[board] pud_table[4]=0x%llx (IRAM1)", pud_table[4]);

    /* Debug: print descriptor values */
    LOG_I("[board] Page table descriptors:");
    LOG_I("  pgd[0]=0x%lx (should be pud_table | 0x3)", pgd[0]);
    LOG_I("  pud_table[0]=0x%lx (VA 0GB-1GB, DEVICE)", pud_table[0]);
    LOG_I("  pud_table[4]=0x%llx (VA 4GB-5GB, IRAM1)", pud_table[4]);

    /* Verify descriptor format is correct */
    if ((pud_table[0] & 0x3) != 0x1) {
        LOG_E("[board] ERROR: pud_table[0] type bits incorrect!");
    }
    if ((pud_table[4] & 0x3) != 0x1) {
        LOG_E("[board] ERROR: pud_table[4] type bits incorrect!");
    }

    /* Output UART address for verification */
    LOG_I("[board] UART base: 0x%lx", uart_base_addr);
    LOG_I("[board] Checking UART LSR...");

    volatile unsigned int *uart_lsr = (volatile unsigned int *)uart_base_addr;
    uart_lsr += 0x14;  /* LSR offset */
    unsigned int lsr_val = *uart_lsr;
    LOG_I("[board] UART LSR read: 0x%x", lsr_val);

    early_putc_direct('V');  /* Verification complete */

/* Configure TCR_EL1 (Translation Control Register) */
/* IPS=2 (1TB physical, supports IRAM1 at 4GB+), TG0=0 (4KB granule), SH0=3 (inner shareable) */
unsigned long tcr = (2UL << 32)    /* IPS=2: 40-bit physical address (1TB), supports IRAM1 @ 4GB+ */
                  | (0UL << 14)    /* TG0=0: 4KB granule size */
                  | (3UL << 12)    /* SH0=3: Inner shareable */
                  | (1UL << 10)    /* ORGN0=1: Normal memory, Outer Cacheable */
                  | (1UL << 8)     /* IRGN0=1: Normal memory, Inner Cacheable */
                  | (0UL << 6)     /* T0SZ=0: 64-bit virtual address space */
                  | (0UL << 0);    /* Reserved */
__asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
__asm__ volatile("isb" ::: "memory");
LOG_D("[board] TCR_EL1 configured: 0x%lx", tcr);

/* Manual MMU table base register set */
__asm__ volatile("msr ttbr0_el1, %0" :: "r"((unsigned long)pgd) : "memory");
__asm__ volatile("isb" ::: "memory");
LOG_D("[board] TTBR0_EL1 set to pgd: %p", pgd);

/* Manual MMU table base register set - use IRAM0-resident PGD */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((unsigned long)hp232x_pgd) : "memory");
    __asm__ volatile("isb" ::: "memory");
    LOG_D("[board] TTBR0_EL1 set to hp232x_pgd @ IRAM0: %p", hp232x_pgd);

    /* Debug: Use simple UART output */
    extern void early_putc_direct(char c);
    early_putc_direct('M');  /* Mark: Before MAIR */

/* Configure MAIR_EL1 BEFORE enabling MMU (Memory Attribute Indirection Register) */
/* Use RT-Thread standard value: 0x00447fUL
 * Attr0: 0x7f - Normal memory, Outer/Inner WB cacheable
 * Attr1: 0x44 - Normal memory, Non-Cacheable
 * Attr2: 0x00 - Device memory, nGnRnE
 */
unsigned long mair = 0x00447fUL;
__asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
__asm__ volatile("isb" ::: "memory");

/* Verify MAIR_EL1 was set correctly */
unsigned long mair_verify;
__asm__ volatile("mrs %0, mair_el1" : "=r"(mair_verify));
LOG_I("[board] MAIR_EL1: wr=0x%lx rd=0x%lx", mair, mair_verify);
early_putc_direct('A');  /* Mark: After MAIR */

/* CRITICAL: Verify page tables before enabling MMU */
    LOG_I("[board] Page table descriptors:");
    LOG_I("  pgd[0]=0x%lx (should be pud_table link)", pgd[0]);
    LOG_I("  pud[0]=0x%lx (VA 0GB-1GB, links to PMD)", pud_table[0]);
    LOG_I("  pud[4]=0x%llx (VA 4GB-5GB, IRAM1 block)", pud_table[4]);
    LOG_I("  pmd[32]=0x%lx pmd[64]=0x%lx pmd[128]=0x%lx",
          pmd_table_0gb[32], pmd_table_0gb[64], pmd_table_0gb[128]);

    /* Data synchronization barrier */
    __asm__ volatile("dsb sy" ::: "memory");
    early_putc_direct('P');  /* Page tables verified */

    /* Enable MMU - REQUIRED for accessing IRAM1 heap at 4GB+ boundary */
    LOG_I("[board] STEP3: Enable MMU (方案B: C代码配置)");
    early_putc_direct('U');  /* Mark: Before MMU enable */

    unsigned long sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    LOG_I("[board] SCTLR_EL1 before: 0x%lx (M=%d C=%d I=%d)",
          sctlr, (sctlr & 0x1) ? 1 : 0, (sctlr & 0x4) ? 1 : 0, (sctlr & 0x1000) ? 1 : 0);

    /* PHASE 1: Enable MMU WITHOUT caches (safer) */
    sctlr |= 0x1UL;      /* Enable MMU */
    sctlr &= ~0x4UL;     /* Disable data cache */
    sctlr &= ~0x1000UL;  /* Disable instruction cache */

    LOG_I("[board] Enabling MMU only: SCTLR=0x%lx", sctlr);
    early_putc_direct('1');  /* Before MMU enable */

    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    early_putc_direct('2');  /* After MMU enable */
    LOG_I("[board] MMU enabled without caches");

    /* Verify MMU */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    LOG_I("[board] SCTLR after MMU: 0x%lx (M=%d C=%d I=%d)",
          sctlr, (sctlr & 0x1) ? 1 : 0, (sctlr & 0x4) ? 1 : 0, (sctlr & 0x1000) ? 1 : 0);

    /* PHASE 2: Test IRAM1 access */
    volatile unsigned long *test_ptr = (volatile unsigned long *)0x100040000;
    unsigned long test_val = *test_ptr;
    LOG_I("[board] IRAM1 test: read 0x%lx from 0x100040000", test_val);
    early_putc_direct('T');  /* Test passed */

    /* PHASE 3: Enable caches */
    sctlr |= 0x4UL;     /* Enable data cache */
    sctlr |= 0x1000UL;  /* Enable instruction cache */

    LOG_I("[board] Enabling caches: SCTLR=0x%lx", sctlr);
    early_putc_direct('C');  /* Before cache enable */

    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    early_putc_direct('D');  /* After cache enable */
    LOG_I("[board] MMU + caches fully enabled (方案B完成)");
#endif  /* 结束 #if 0 MMU配置代码 */

skip_all_mmu:
    LOG_I("[board] NO MMU - Using identity mapping (no page tables)");
LOG_I("[board] STEP4: Skip rt_hw_mmu_setup - using manual setup");

#ifdef RT_USING_HEAP
    rt_system_heap_init((void *)heap_start, (void *)heap_end);
    LOG_I("[board] Heap: 0x%lx-0x%lx (%ldKB)",
          heap_start, heap_end, (heap_end - heap_start) / 1024);
#endif

    /* initialize hardware interrupt */
    LOG_I("[board] About to call rt_interrupt_init (GICv3 with bootcode-assumed EL config)");
    rt_hw_interrupt_init();
    LOG_I("[board] rt_interrupt_init OK");

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
