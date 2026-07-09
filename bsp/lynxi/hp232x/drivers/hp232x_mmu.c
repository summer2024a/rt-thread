#define DBG_TAG "hp232x_mmu"
#define DBG_LVL DBG_LOG

#include <rtdbg.h>
#include <rtthread.h>
#include <rthw.h>
#include <stdint.h>

#include "board.h"
#include "hp232x_mmu.h"

static uint64_t hp232x_mmu_l1[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".bss.noclean.mmu_table")));
static uint64_t hp232x_mmu_l2_low[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".bss.noclean.mmu_table")));
static uint64_t hp232x_mmu_l2_ram1[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".bss.noclean.mmu_table")));
static uint64_t hp232x_mmu_l3_ram0[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".bss.noclean.mmu_table")));
static uint64_t hp232x_mmu_l3_ram1[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".bss.noclean.mmu_table")));

static inline uint64_t hp232x_desc_table(uint64_t addr)
{
    return (addr & ADDR_MASK_TABLE) | DESC_TABLE;
}

static inline uint64_t hp232x_desc_block(uint64_t addr, uint64_t attr)
{
    return (addr & ADDR_MASK_L2_BLOCK) | attr | DESC_BLOCK;
}

static inline uint64_t hp232x_desc_l1_block(uint64_t addr, uint64_t attr)
{
    return (addr & ADDR_MASK_L1_BLOCK) | attr | DESC_BLOCK;
}

static inline uint64_t hp232x_desc_page(uint64_t addr, uint64_t attr)
{
    return (addr & ADDR_MASK_L3_PAGE) | attr | DESC_PAGE;
}

static void hp232x_map_l2_block_range(uint64_t *table,
                                      uint64_t va_start,
                                      uint64_t pa_start,
                                      uint64_t size,
                                      uint64_t attr)
{
    uint64_t page_count = size >> 21;
    uint64_t index = (va_start >> 21) & 0x1ffUL;

    for (uint64_t i = 0; i < page_count; i++)
    {
        table[index + i] = hp232x_desc_block(pa_start + (i << 21), attr);
    }
}

static void hp232x_map_l3_page_range(uint64_t *table,
                                     uint64_t pa_start,
                                     uint64_t size,
                                     uint64_t attr)
{
    uint64_t page_count = size >> 12;
    uint64_t paddr = pa_start;

    for (uint64_t i = 0; i < page_count; i++)
    {
        table[i] = hp232x_desc_page(paddr + (i << 12), attr);
    }
}

static void hp232x_map_l1_device(uint64_t pa_base)
{
    hp232x_mmu_l1[pa_base >> 30] = hp232x_desc_l1_block(pa_base, ATTR_DEVICE);
}

static void hp232x_map_apu_regions(void)
{
    static const uint64_t apu_bases[] =
    {
        HP232X_APU_NORM_BASE,      /* L1[80]: NORM/CR/MDBG */
        HP232X_APU_CORECFG0_BASE,  /* L1[84]: apu_hw_init fix reg low */
        HP232X_APU_CORECFG1_BASE,  /* L1[85]: apu_hw_init fix reg high */
        HP232X_APU_NN_FIFO0_BASE,  /* L1[88]: SLV0 NN FIFO ch0-3 */
        HP232X_APU_NN_FIFO1_BASE,  /* L1[89]: SLV0 NN FIFO ch4-7 */
    };
    rt_size_t i;

    for (i = 0; i < sizeof(apu_bases) / sizeof(apu_bases[0]); i++)
    {
        hp232x_map_l1_device(apu_bases[i]);
    }
}

static void hp232x_build_page_tables(void)
{
    rt_memset(hp232x_mmu_l1, 0, sizeof(hp232x_mmu_l1));
    rt_memset(hp232x_mmu_l2_low, 0, sizeof(hp232x_mmu_l2_low));
    rt_memset(hp232x_mmu_l2_ram1, 0, sizeof(hp232x_mmu_l2_ram1));
    rt_memset(hp232x_mmu_l3_ram0, 0, sizeof(hp232x_mmu_l3_ram0));
    rt_memset(hp232x_mmu_l3_ram1, 0, sizeof(hp232x_mmu_l3_ram1));

    hp232x_mmu_l1[0] = hp232x_desc_table((uint64_t)hp232x_mmu_l2_low);
    hp232x_mmu_l1[HP232X_MMU_L1_INDEX_RAM1] = hp232x_desc_table((uint64_t)hp232x_mmu_l2_ram1);
    hp232x_map_apu_regions();

    hp232x_map_l2_block_range(hp232x_mmu_l2_low,
                              0x00000000UL,
                              0x00000000UL,
                              0x00200000UL,
                              ATTR_DEVICE);

    hp232x_map_l2_block_range(hp232x_mmu_l2_low,
                              0x02000000UL,
                              0x02000000UL,
                              0x00200000UL,
                              ATTR_DEVICE);

    hp232x_mmu_l2_low[32] = hp232x_desc_table((uint64_t)hp232x_mmu_l3_ram0);
    hp232x_map_l3_page_range(hp232x_mmu_l3_ram0,
                             IRAM0_START,
                             IRAM0_SIZE,
                             ATTR_NORMAL_WB);

    hp232x_map_l2_block_range(hp232x_mmu_l2_low,
                              0x06000000UL,
                              0x06000000UL,
                              0x00200000UL,
                              ATTR_DEVICE);

    hp232x_map_l2_block_range(hp232x_mmu_l2_low,
                              0x08000000UL,
                              0x08000000UL,
                              0x08000000UL,
                              ATTR_DEVICE);

    hp232x_map_l2_block_range(hp232x_mmu_l2_low,
                              0x10000000UL,
                              0x10000000UL,
                              0x10000000UL,
                              ATTR_DEVICE);

    hp232x_mmu_l2_ram1[0] = hp232x_desc_table((uint64_t)hp232x_mmu_l3_ram1);
    hp232x_map_l3_page_range(hp232x_mmu_l3_ram1,
                             IRAM1_START,
                             IRAM1_SIZE,
                             ATTR_NORMAL_WB);
}

void hp232x_mmu_init(void)
{
    uint64_t tcr;
    uint64_t sctlr;
    uint64_t mair = 0x000044ffUL;

    early_putc_direct('1');  /* Step 1: build page tables */
    hp232x_build_page_tables();
    early_putc_direct('2');  /* Step 2: MAIR configured */

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("isb" ::: "memory");

    early_putc_direct('3');  /* Step 3: TCR configured */
    tcr = 25UL;                 /* T0SZ = 25 (39-bit VA) */
    tcr |= (1UL << 8);          /* IRGN0 = WB WA RA */
    tcr |= (1UL << 10);         /* ORGN0 = WB WA RA */
    tcr |= (3UL << 12);         /* SH0 = Inner Shareable */
    tcr |= (2UL << 32);         /* IPS = 40-bit PA */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("isb" ::: "memory");

    early_putc_direct('4');  /* Step 4: TTBR0 set */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)hp232x_mmu_l1) : "memory");
    __asm__ volatile("isb" ::: "memory");

    early_putc_direct('5');  /* Step 5: TLB invalidate + MMU enable */
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 0x1UL;             /* enable MMU */
    sctlr &= ~0x4UL;            /* disable data cache */
    sctlr &= ~0x1000UL;         /* disable instruction cache */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    /* Note: early_putc_direct may hang after MMU enable due to UART state */
    /* Continue without printing marker 6/7 */
    volatile uint32_t *iram1_test = (volatile uint32_t *)IRAM1_USE_START;
    (void)*iram1_test;
    early_putc_direct('7');  /* Step 7: IRAM1 test done */

    /* === CRITICAL: Full TLB + cache maintenance after MMU enable (prevents SError on first eret) === */
    __asm__ volatile("tlbi vmalle1" ::: "memory");   /* Invalidate all TLB */
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("dc civac, %0" : : "r"(0) : "memory"); /* D-cache clean to PoC (any pending dirty) */
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("ic ialluis" ::: "memory");     /* Invalidate all I-cache */
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] Enable caches (D + I)");
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 0x4UL;             /* enable data cache */
    sctlr |= 0x1000UL;          /* enable instruction cache */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] HP232X MMU init complete");
}

uint64_t *hp232x_mmu_get_l1_table(void)
{
    return hp232x_mmu_l1;
}

void hp232x_mmu_secondary_init(void)
{
    uint64_t tcr;
    uint64_t sctlr;
    uint64_t mair = 0x000044ffUL;

    /* Page tables were built on CPU0; ensure CPU1 sees coherent table contents. */
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l1, sizeof(hp232x_mmu_l1));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l2_low, sizeof(hp232x_mmu_l2_low));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l2_ram1, sizeof(hp232x_mmu_l2_ram1));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l3_ram0, sizeof(hp232x_mmu_l3_ram0));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l3_ram1, sizeof(hp232x_mmu_l3_ram1));
    rt_hw_barrier(dsb, sy);

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("isb" ::: "memory");

    tcr = 25UL;
    tcr |= (1UL << 8);
    tcr |= (1UL << 10);
    tcr |= (3UL << 12);
    tcr |= (2UL << 32);
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)hp232x_mmu_l1) : "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    if (!(sctlr & 0x1UL))
    {
        sctlr |= 0x1UL;
        sctlr &= ~0x4UL;
        sctlr &= ~0x1000UL;
        __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
        __asm__ volatile("isb" ::: "memory");

        __asm__ volatile("tlbi vmalle1" ::: "memory");
        __asm__ volatile("dsb sy" ::: "memory");
        __asm__ volatile("ic ialluis" ::: "memory");
        __asm__ volatile("dsb sy" ::: "memory");
        __asm__ volatile("isb" ::: "memory");
    }

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 0x4UL;
    sctlr |= 0x1000UL;
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] Secondary CPU MMU init complete (TTBR0=%p SCTLR=0x%llx)",
          (void *)hp232x_mmu_l1, sctlr);
}
