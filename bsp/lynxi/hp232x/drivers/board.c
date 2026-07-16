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
#include "biz_log.h"
#include <smp_call.h>
#include <rthw.h>
#include <rtthread.h>
#include <mm_aspace.h>
#include <cpuport.h>

#include <rtconfig.h>
#include "board.h"
#include "drv_uart.h"
#include "drv_apb_timer.h"
#include "hp232x_mmu.h"
#ifdef BSP_USING_SYSCTL_CLK
#include "clock/drv_sysctl_lite.h"
#endif

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

extern int __bss_start;
extern char __dma_nocache_start[];
extern char __dma_nocache_end[];
extern int __bss_end;
extern void *system_vectors;

static int is_uart_initialized = 0;

#ifdef BSP_USING_HP232X_ARCH_TIMER_PROBE
static rt_uint64_t hp232x_read_cntpct(void)
{
    rt_uint64_t v;

    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static rt_uint64_t hp232x_read_cntfrq(void)
{
    rt_uint64_t v;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void hp232x_core_timer_platform_init(void)
{
    volatile rt_uint32_t *base = (volatile rt_uint32_t *)0x08600000UL;

    base[0] = 0x1;
    base[2] = 0xf;
    base[3] = 0xf;
    base[8] = 31250000U;

    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static rt_uint64_t hp232x_arch_timer_sample_delta(void)
{
    rt_uint64_t start = hp232x_read_cntpct();
    rt_uint64_t end;
    volatile int i;

    for (i = 0; i < 2000000; i++)
    {
        __asm__ volatile("nop");
    }

    end = hp232x_read_cntpct();
    return end - start;
}

#define DW_TIMER_CONTROL_ENABLE     (1U << 0)
#define DW_TIMER_CONTROL_MODE       (1U << 1)

static rt_uint64_t hp232x_arch_timer_wall_delta(void)
{
    const rt_uint32_t probe_id = 1u;
    const rt_uint32_t apb_ticks = HP232X_APB_TIMER_CLOCK / 10u;
    volatile rt_uint32_t *base = (volatile rt_uint32_t *)(rt_ubase_t)STIMER_BASE;
    rt_uint32_t load_off = probe_id * 5u;
    rt_uint32_t cur_off = load_off + 1u;
    rt_uint32_t ctl_off = load_off + 2u;
    rt_uint32_t apb_start;
    rt_uint32_t apb_now;
    rt_uint32_t elapsed;
    rt_uint64_t cnt_start;
    rt_uint64_t cnt_end;
    rt_uint32_t guard;

    base[ctl_off] = 0u;
    base[load_off] = 0xffffffffu;
    base[ctl_off] = DW_TIMER_CONTROL_ENABLE;

    apb_start = base[cur_off];
    cnt_start = hp232x_read_cntpct();

    guard = 0u;
    do
    {
        apb_now = base[cur_off];
        elapsed = apb_start - apb_now;
        guard++;
        if (guard > 100000000u)
        {
            break;
        }
    } while (elapsed < apb_ticks);

    cnt_end = hp232x_read_cntpct();
    base[ctl_off] = 0u;

    return cnt_end - cnt_start;
}

static rt_uint64_t hp232x_probe_pre_delta;
static rt_uint64_t hp232x_probe_post_delta;
static rt_uint64_t hp232x_probe_wall_delta;
#endif /* BSP_USING_HP232X_ARCH_TIMER_PROBE */

size_t gpio_base_addr = GPIO_BASE_ADDR;
size_t uart_base_addr = UART_BASE;
size_t gic_base_addr  = GIC_V2_BASE;
size_t arm_timer_base = ARM_TIMER_BASE;
size_t stimer_base_addr = STIMER_BASE;
size_t wdt_base_addr = WDT_BASE;

/* UART breadcrumb for BOOT_EARLY / SMP_EARLY / trap debug — always linked. */
void early_putc_direct(char c)
{
    volatile unsigned int *uart_thr = (volatile unsigned int *)0x10006000;
    volatile unsigned int *uart_lsr = (volatile unsigned int *)0x10006014;

    while ((*uart_lsr & 0x40) == 0);
    *uart_thr = (unsigned int)c;
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
// struct mem_desc platform_mem_desc[] = {
//     /* IRAM0 — identity mapping at 0x04000000 (first 256KB usable) */
//     {IRAM0_START, IRAM0_USE_END - 1, IRAM0_START, NORMAL_MEM},
//     /* IRAM1 — identity mapping at 0x100040000 (last 256KB usable) */
//     {IRAM1_USE_START, IRAM1_END - 1, IRAM1_USE_START, NORMAL_MEM},
//     /* Peripheral space (GIC, UART, timer, etc.) */
//     {INTC_BASE, INTC_BASE + 0x14000000UL - 1, INTC_BASE, DEVICE_MEM},
// };

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

// const rt_uint32_t platform_mem_desc_size = sizeof(platform_mem_desc)/sizeof(platform_mem_desc[0]);

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
    /* Boot breadcrumbs: enable BSP_BOOT_EARLY_MARK (see board.h BOOT_EARLY_PUTC) */
    BOOT_EARLY_PUTC('\n');
    BOOT_EARLY_PUTC('B');
    BOOT_EARLY_PUTC('O');
    BOOT_EARLY_PUTC('O');
    BOOT_EARLY_PUTC('T');
    BOOT_EARLY_PUTC('\n');

    rt_ubase_t bss_end;
    rt_ubase_t noclean_end;
    rt_ubase_t bss_start;
    rt_ubase_t page_start;
    rt_ubase_t page_end;
    rt_ubase_t heap_start;
    rt_ubase_t heap_end;
    rt_ubase_t stack_guard_base;
    rt_ubase_t dma_nc_start = (rt_ubase_t)&__dma_nocache_start;
    rt_ubase_t dma_nc_end = (rt_ubase_t)&__dma_nocache_end;

    /* .dma_nocache @ 0x100040000 is below __bss_start — zero before MMU/DMA use */
    if (dma_nc_end > dma_nc_start)
        rt_memset((void *)dma_nc_start, 0, dma_nc_end - dma_nc_start);

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

    /* Clamp page/heap to stack guard — BSS growth must not spill past IRAM1 */
    if (page_end > stack_guard_base)
        page_end = page_start;
    if (heap_end > stack_guard_base)
        heap_end = stack_guard_base;
    if (heap_start > heap_end)
        heap_start = heap_end;

    RT_ASSERT(noclean_end >= IRAM1_USE_START);
    RT_ASSERT(bss_start >= IRAM1_USE_START);
    RT_ASSERT(heap_end <= IRAM1_END);
    RT_ASSERT(heap_end <= stack_guard_base);
    RT_ASSERT(heap_start <= heap_end);

    if ((heap_end - heap_start) < HEAP_POOL_SIZE / 2U)
    {
        LOG_W("[board] IRAM1 tight: bss_end=0x%lx heap=[0x%lx,0x%lx) size=%lu",
              (unsigned long)bss_end,
              (unsigned long)heap_start, (unsigned long)heap_end,
              (unsigned long)(heap_end - heap_start));
    }

    init_page_region.start = page_start;
    init_page_region.end = page_end;

    rt_hw_earlycon_ioremap_early();
    BOOT_EARLY_PUTC('E');  /* E for earlycon ioremap done */

    LOG_I("[board] IRAM1 layout bss_end=0x%lx page=[0x%lx,0x%lx) heap=[0x%lx,0x%lx)",
          (unsigned long)bss_end,
          (unsigned long)page_start, (unsigned long)page_end,
          (unsigned long)heap_start, (unsigned long)heap_end);
    BOOT_EARLY_PUTC('I');  /* I for IRAM1 info */
    /* Simplified: just print marker, skip full address dump for now */
    BOOT_EARLY_PUTC('M');  /* M for MMU init start */
    LOG_I("[board] STEP1: HP232X MMU initialization");
    hp232x_mmu_init();
    BOOT_EARLY_PUTC('R');  /* R for MMU init returned */

    LOG_I("[board] MMU initialization complete - proceeding to heap setup");

#ifdef BSP_USING_HP232X_ARCH_TIMER_PROBE
    hp232x_probe_pre_delta = hp232x_arch_timer_sample_delta();
    BOOT_EARLY_PUTC('p');
#endif

#ifdef BSP_USING_SYSCTL_CLK
    lynxi_sysctl_lite_init();
    BOOT_EARLY_PUTC('s');
    LOG_I("[board] sysctl: PLL boot select + cpu_timer + fabric gates");
#endif

#ifdef BSP_USING_HP232X_ARCH_TIMER_PROBE
    hp232x_probe_post_delta = hp232x_arch_timer_sample_delta();
    BOOT_EARLY_PUTC('q');
#endif

#ifdef RT_USING_HEAP
    if (heap_end > heap_start)
        rt_system_heap_init((void *)heap_start, (void *)heap_end);
    else
        LOG_E("[board] heap unavailable — IRAM1 BSS overflow");
#endif

    LOG_I("[board] starting GICv3 init...");

    /* Initialize GIC */
    rt_hw_interrupt_init();

    /* NOTE: GIC Secure configuration (ARE_NS, UART Group1) is done in pre_entry.S at EL3 */
    /* EL1 Non-Secure cannot modify GIC Secure registers - these attempts will fail */

    /* Verify GIC configuration (read-only at EL1 NS) */
    HP_LOGI("===== Verifying GIC configuration =====\n");

    volatile uint32_t *gicd_ctrl = (volatile uint32_t *)(GIC_PL500_DISTRIBUTOR_PPTR + 0x000);
    uint32_t gicd_ctlr = *gicd_ctrl;
    HP_LOGI("  GICD_CTLR: 0x%x (ARE_NS view at EL1 NS)\n", gicd_ctlr);

    HP_LOGI("========================================\n");

    /* Check ICC_IGRPEN1_EL1 (Interrupt Group Enable) */
    uint64_t igprpen1;
    __asm__ volatile("mrs %0, S3_0_C12_C12_7" : "=r"(igprpen1));
    LOG_I("  ICC_IGRPEN1_EL1 = 0x%llx (Enable=%d)", igprpen1, (igprpen1 >> 0) & 1);

    LOG_I("[board] ===== GICv3 Initialization Complete =====");

    /* initialize uart */
    rt_hw_uart_init();
    is_uart_initialized = 1;
    LOG_I("-->rt_hw_uart_init ok\n");

#ifdef BSP_USING_HP232X_ARCH_TIMER_PROBE
    {
        rt_uint64_t ratio_x1000;

        hp232x_core_timer_platform_init();
        HP_LOGI("[arch-timer] CNTFRQ=%llu Hz\n",
                   (unsigned long long)hp232x_read_cntfrq());
#ifdef BSP_USING_SYSCTL_CLK
        HP_LOGI("[arch-timer] BOOT_SELECT before=0x%08x after=0x%08x\n",
                   (unsigned int)lynxi_sysctl_lite_boot_select_before,
                   (unsigned int)lynxi_sysctl_lite_boot_select_after);
        HP_LOGI("[arch-timer] CPR+0x64=0x%08x CPR+0x68=0x%08x CPR+0xd4=0x%08x\n",
                   (unsigned int)lynxi_sysctl_lite_reg_read(0x64u),
                   (unsigned int)lynxi_sysctl_lite_reg_read(0x68u),
                   (unsigned int)lynxi_sysctl_lite_reg_read(0xd4u));
#endif
        HP_LOGI("[arch-timer] pre-sysctl  CNTPCT delta=%llu (2M nop)\n",
                   (unsigned long long)hp232x_probe_pre_delta);
        HP_LOGI("[arch-timer] post-sysctl CNTPCT delta=%llu (2M nop)\n",
                   (unsigned long long)hp232x_probe_post_delta);
        if (hp232x_probe_pre_delta)
        {
            ratio_x1000 = hp232x_probe_post_delta * 1000 / hp232x_probe_pre_delta;
            HP_LOGI("[arch-timer] nop post/pre ratio=%llu.%03llu",
                       (unsigned long long)(ratio_x1000 / 1000),
                       (unsigned long long)(ratio_x1000 % 1000));
            if (ratio_x1000 > 50000)
            {
                HP_LOGI(" (PLL switch raised CNTPCT rate ~62x)\n");
            }
            else
            {
                HP_LOGI(" (nop window unchanged by sysctl)\n");
            }
        }
        else
        {
            HP_LOGI("\n");
        }
    }
#endif

    /* initialize timer for os tick */
#ifdef BSP_USING_APB_TIMER_AS_TICK
    rt_hw_apb_timer_init();
    LOG_I("-->rt_hw_apb_timer_init ok\n");
#elif defined(BSP_USING_CORETIMER)
    rt_hw_gtimer_init();
    LOG_I("-->rt_hw_gtimer_init ok\n");
#endif

#ifdef BSP_USING_HP232X_ARCH_TIMER_PROBE
    hp232x_probe_wall_delta = hp232x_arch_timer_wall_delta();
    HP_LOGI("[arch-timer] wall 100ms CNTPCT delta=%llu (expect ~3125000 or ~50000)\n",
               (unsigned long long)hp232x_probe_wall_delta);
    HP_LOGI("[arch-timer] wall CNTPCT rate ~%llu Hz (expect ~31250000)\n",
               (unsigned long long)(hp232x_probe_wall_delta * 10));
#endif

#ifdef RT_USING_CONSOLE
    /* set console device */
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
    LOG_I("-->rt_console_set_device ok\n");
#endif /* RT_USING_CONSOLE */

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif

#ifdef RT_USING_SMP
    LOG_I("[board] Initializing SMP...");

    rt_smp_call_init();

    rt_hw_ipi_handler_install(RT_SCHEDULE_IPI, rt_scheduler_ipi_handler);
    rt_hw_ipi_handler_install(RT_STOP_IPI, rt_scheduler_ipi_handler);
    rt_hw_ipi_handler_install(RT_SMP_CALL_IPI, rt_smp_call_ipi_handler);

    rt_hw_interrupt_umask(RT_SCHEDULE_IPI);
    rt_hw_interrupt_umask(RT_STOP_IPI);
    rt_hw_interrupt_umask(RT_SMP_CALL_IPI);

    LOG_I("[board] SMP initialization complete");
#else
    LOG_I("[board] Non-SMP mode");
#endif

    LOG_I("[board] ===== BOARD INIT COMPLETE =====");
    LOG_I("[board] Next: rt_show_version() → rt_application_init()");
}

#ifdef RT_USING_SMP
#include <gic.h>

void rt_hw_mmu_ktbl_set(unsigned long tbl);
void _secondary_cpu_entry(void);

#ifdef BSP_USING_HP232X
static void hp232x_smp_secondary_cpu_prepare(int cpu_id)
{
    /*
     * No rt_kprintf/LOG here: secondary later holds _cpus_lock through
     * MMU/GIC; printing under that lock deadlocks with CPU0 schedule +
     * THREADSAFE_PRINTF (seen: release OK, never "CPU1 ready").
     */
    (void)cpu_id;

    rt_hw_vector_init();

    rt_hw_sysreg_read(mpidr_el1, rt_cpu_mpidr_table[cpu_id]);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
                         &rt_cpu_mpidr_table[cpu_id],
                         sizeof(rt_cpu_mpidr_table[cpu_id]));
    rt_hw_barrier(dsb, sy);
}
#endif /* BSP_USING_HP232X */

/*
 * Mailbox addresses for secondary CPUs.
 *
 * bootwrapper: mbox_address = 0x401fff0; spin.S offset (linear_id-1)*8.
 * Use HP232X_CPU_RELEASE_MBOX(cpu_id); release loop bound by RT_CPUS_NR.
 */

/* Rewrite spin-table + SEV (safe to call again while polling for online). */
void hp232x_smp_release_cpu(int cpu)
{
    volatile rt_uint64_t *mbox;
    rt_uint64_t entry = (rt_uint64_t)_secondary_cpu_entry;

    if (cpu <= 0 || cpu >= RT_CPUS_NR)
        return;

    mbox = (volatile rt_uint64_t *)(uintptr_t)HP232X_CPU_RELEASE_MBOX(cpu);
    *mbox = entry;
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)mbox, sizeof(*mbox));
    rt_hw_barrier(dsb, sy);
    rt_hw_sev();
}

void rt_hw_secondary_cpu_up(void)
{
    int i;
    volatile rt_uint64_t *mbox;

    /*
     * Do NOT mdelay()/kprintf here around release: secondary holds _cpus_lock
     * until scheduler_start; CPU0 schedule or THREADSAFE_PRINTF can deadlock
     * (Flash cold-boot shows msh then hang mid "Hi, this is"). Wait/poll for
     * online in main() with busy-wait only.
     */
    for (i = 1; i < RT_CPUS_NR; ++i)
    {
        mbox = (volatile rt_uint64_t *)(uintptr_t)HP232X_CPU_RELEASE_MBOX(i);
        *mbox = 0;
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)mbox, sizeof(*mbox));
    }

    rt_hw_barrier(dsb, sy);

    for (i = 1; i < RT_CPUS_NR; ++i)
    {
        /* Bootwrapper WFE may race first SEV; rewrite + second SEV. */
        hp232x_smp_release_cpu(i);
        hp232x_smp_release_cpu(i);
    }
}

void rt_hw_secondary_cpu_bsp_start(void)
{
    int cpu_id = rt_hw_cpu_id();

    /*
     * Under _cpus_lock: never rt_kprintf/LOG (deadlocks CPU0).
     * BSP_SMP_EARLY_MARK: a=entry b=locked c=mmu d=gic e=sched
     */
#ifdef BSP_SMP_EARLY_MARK
    early_putc_direct('a');
#endif

    system_vectors_init();
#ifdef BSP_USING_HP232X
    hp232x_smp_secondary_cpu_prepare(cpu_id);
#endif

    rt_hw_spin_lock(&_cpus_lock);
#ifdef BSP_SMP_EARLY_MARK
    early_putc_direct('b');
#endif

#ifndef BSP_USING_HP232X
    rt_hw_sysreg_read(mpidr_el1, rt_cpu_mpidr_table[cpu_id]);
#endif

    hp232x_mmu_secondary_init();
#ifdef BSP_SMP_EARLY_MARK
    early_putc_direct('c');
#endif

#ifdef BSP_USING_GICV3
#ifdef BSP_USING_HP232X
    arm_gic_cpu_init(0, 0);
    arm_gic_redist_init(0, GIC_PL500_REDISTRIBUTOR_PPTR);
#else
    arm_gic_redist_init(0, GIC_PL500_REDISTRIBUTOR_PPTR);
    arm_gic_cpu_init(0, 0);
#endif
#endif
#ifdef BSP_SMP_EARLY_MARK
    early_putc_direct('d');
#endif

#if defined(BSP_USING_HP232X) && defined(BSP_USING_APB_TIMER_AS_TICK)
    /*
     * Tick is APB timer on CPU0 only. Do not enable CNTP PPI 30 on secondary
     * CPUs — spurious timer IRQs block IPI-driven scheduling on CPU1.
     */
#elif !defined(RT_CLOCK_TIME_ARM_ARCH)
    rt_hw_gtimer_local_enable();
#endif

    rt_hw_interrupt_umask(RT_SCHEDULE_IPI);
    rt_hw_interrupt_umask(RT_STOP_IPI);
    rt_hw_interrupt_umask(RT_SMP_CALL_IPI);

    (void)cpu_id;

#ifdef BSP_SMP_EARLY_MARK
    early_putc_direct('e');
#endif
    rt_system_scheduler_start();
}

void rt_hw_secondary_cpu_idle_exec(void)
{
    /*
     * First idle entry ⇒ scheduler_start + context switch succeeded.
     * Must schedule before WFE: IPI alone is not enough if the ready-queue
     * update raced with idle.
     */
#ifdef BSP_SMP_EARLY_MARK
    static volatile int s_idle_mark;

    if (!s_idle_mark)
    {
        s_idle_mark = 1;
        early_putc_direct('I');
    }
#endif
    rt_schedule();
    rt_hw_wfe();
}

#ifdef BSP_USING_HP232X
void hp232x_kick_cpu(int cpu)
{
    if (cpu <= 0 || cpu >= RT_CPUS_NR)
        return;

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, rt_cpu_index(cpu), sizeof(struct rt_cpu));
    rt_hw_barrier(dsb, sy);
    rt_hw_ipi_send(RT_SCHEDULE_IPI, 1U << (unsigned)cpu);
    rt_hw_sev();
}
#endif

#endif
