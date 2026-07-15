#include "biz_log.h"
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

#ifdef BSP_IRAM0_LOW_NC
    /*
     * Host biz descriptors / buffers: IRAM0 low 256KB → Normal NC.
     * BL22 kernel stays in high half (WB). Includes secondary mbox @ 0x401fff0.
     */
    {
        uint64_t nc_start = IRAM0_LOW_START;
        uint64_t nc_size  = IRAM0_LOW_SIZE;
        uint64_t page_idx = (nc_start - IRAM0_START) >> 12;
        uint64_t page_cnt = nc_size >> 12;
        uint64_t i;

        for (i = 0; i < page_cnt; i++)
        {
            hp232x_mmu_l3_ram0[page_idx + i] =
                hp232x_desc_page(nc_start + (i << 12), ATTR_NORMAL_NC);
        }
    }
#endif

    hp232x_map_l2_block_range(hp232x_mmu_l2_low,
                              0x06000000UL,
                              0x06000000UL,
                              0x00200000UL,
                              ATTR_DEVICE);

    /*
     * Boot SSI win=1 → PA0. Linux ioremap(0) allocates a *high VA* → PA0;
     * it never touches VA=0. RTT identity map would use regs==NULL/VA0 and
     * SEA (seen on Flash cold boot). Alias matches ioremap semantics.
     */
    hp232x_map_l2_block_range(hp232x_mmu_l2_low,
                              0x07000000UL,
                              0x00000000UL,
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

#ifdef BSP_IRAM1_LOW_NC
    /*
     * Host Load scratch: IRAM1 low 256KB → Normal NC (BL22: no code/stack).
     * ka200_tools DDR_IRAM_ADDR = 0x100000000.
     */
    {
        uint64_t nc_start = IRAM1_HOST_SCRATCH_START;
        uint64_t nc_size  = IRAM1_HOST_SCRATCH_SIZE;
        uint64_t page_idx = (nc_start - IRAM1_START) >> 12;
        uint64_t page_cnt = nc_size >> 12;
        uint64_t i;

        for (i = 0; i < page_cnt; i++)
        {
            hp232x_mmu_l3_ram1[page_idx + i] =
                hp232x_desc_page(nc_start + (i << 12), ATTR_NORMAL_NC);
        }
    }
#endif

#ifndef BSP_EMMC_DMA_CACHED_BSS
    /*
     * ADMA descriptor arena NC @ IRAM1_DMA_NC_START (64KB, .dma_nocache).
     * Separate from Host scratch (IRAM1 low 256KB); placement unchanged.
     * With BSP_EMMC_DMA_CACHED_BSS: leave WB like hp640 SPL BSS + flush_cache.
     */
    {
        uint64_t nc_start = IRAM1_DMA_NC_START;
        uint64_t nc_size  = IRAM1_DMA_NC_SIZE;
        uint64_t page_idx = (nc_start - IRAM1_START) >> 12;
        uint64_t page_cnt = nc_size >> 12;
        uint64_t i;

        for (i = 0; i < page_cnt; i++)
        {
            hp232x_mmu_l3_ram1[page_idx + i] =
                hp232x_desc_page(nc_start + (i << 12), ATTR_NORMAL_NC);
        }
    }
#endif
}

void hp232x_mmu_flush_tables(void)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l1, sizeof(hp232x_mmu_l1));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l2_low, sizeof(hp232x_mmu_l2_low));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l2_ram1, sizeof(hp232x_mmu_l2_ram1));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l3_ram0, sizeof(hp232x_mmu_l3_ram0));
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)hp232x_mmu_l3_ram1, sizeof(hp232x_mmu_l3_ram1));
    __asm__ volatile("dsb sy" ::: "memory");
}

/**
 * Dump L2 block mapping for VA (identity / PA0 alias check).
 * Returns 0 if L2 is a valid BLOCK with expected PA (low 31 bits match).
 */
int hp232x_mmu_check_l2_block(uint64_t va, uint64_t expect_pa,
                              uint64_t *out_desc, uint32_t *out_attr_idx)
{
    uint64_t l1e, l2e;
    uint32_t l1idx, l2idx;
    uint64_t pa;

    l1idx = (uint32_t)((va >> 30) & 0x1ffUL);
    l1e = hp232x_mmu_l1[l1idx];
    if ((l1e & 0x3UL) != DESC_TABLE)
        return -1;

    l2idx = (uint32_t)((va >> 21) & 0x1ffUL);
    if (l1idx == HP232X_MMU_L1_INDEX_RAM1)
        l2e = hp232x_mmu_l2_ram1[l2idx];
    else if (l1idx == 0U)
        l2e = hp232x_mmu_l2_low[l2idx];
    else
        return -2;

    /* L2 block: bits[1:0]==01 ; table==11 */
    if ((l2e & 0x3UL) != DESC_BLOCK)
        return -3;

    pa = l2e & ADDR_MASK_L2_BLOCK;
    if (out_desc)
        *out_desc = l2e;
    if (out_attr_idx)
        *out_attr_idx = (uint32_t)((l2e >> 2) & 0x7UL);

    if ((pa & ~0x1fffffUL) != (expect_pa & ~0x1fffffUL))
        return -4;

    return 0;
}

int hp232x_mmu_pte_attr_index(uint64_t va, uint32_t *out_attr_idx)
{
    uint64_t l1e, l2e, l3e;
    uint32_t l1idx, l2idx, l3idx;

    l1idx = (uint32_t)((va >> 30) & 0x1ffUL);
    l1e = hp232x_mmu_l1[l1idx];
    if ((l1e & 0x3UL) != 0x3UL)
        return -1;

    l2idx = (uint32_t)((va >> 21) & 0x1ffUL);
    if (l1idx == HP232X_MMU_L1_INDEX_RAM1)
        l2e = hp232x_mmu_l2_ram1[l2idx];
    else
        l2e = hp232x_mmu_l2_low[l2idx];
    if ((l2e & 0x3UL) != 0x3UL)
        return -2;

    l3idx = (uint32_t)((va >> 12) & 0x1ffUL);
    if (l1idx == HP232X_MMU_L1_INDEX_RAM1)
        l3e = hp232x_mmu_l3_ram1[l3idx];
    else
        return -3;

    if ((l3e & 0x3UL) != 0x3UL)
        return -4;

    if (out_attr_idx)
        *out_attr_idx = (uint32_t)((l3e >> 2) & 0x7UL);

    return 0;
}

int hp232x_mmu_verify_dma_nc(uint64_t test_va)
{
    uint32_t attr_idx = 0xffU;
    volatile uint32_t *probe = (volatile uint32_t *)(uintptr_t)test_va;
    uint32_t save, val;
    int ret;

    ret = hp232x_mmu_pte_attr_index(test_va, &attr_idx);
    if (ret != 0)
        return -10 - ret;

    /* AttrIndx 1 = MAIR NC slot (0x44) used by ATTR_NORMAL_NC */
    if (attr_idx != 1U)
        return -20;

    save = *probe;
    *probe = 0xa5a50001U;
    __asm__ volatile("dsb sy" ::: "memory");
    val = *probe;
    *probe = save;
    __asm__ volatile("dsb sy" ::: "memory");

    if (val != 0xa5a50001U)
        return -21;

    return 0;
}

void hp232x_mmu_init(void)
{
    uint64_t tcr;
    uint64_t sctlr;
    uint64_t mair = 0x000044ffUL;

    BOOT_EARLY_PUTC('1');  /* Step 1: build page tables */
    hp232x_build_page_tables();
    hp232x_mmu_flush_tables();
    BOOT_EARLY_PUTC('2');  /* Step 2: MAIR configured */

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("isb" ::: "memory");

    BOOT_EARLY_PUTC('3');  /* Step 3: TCR configured */
    tcr = 25UL;                 /* T0SZ = 25 (39-bit VA) */
    tcr |= (1UL << 8);          /* IRGN0 = WB WA RA */
    tcr |= (1UL << 10);         /* ORGN0 = WB WA RA */
    tcr |= (3UL << 12);         /* SH0 = Inner Shareable */
    tcr |= (2UL << 32);         /* IPS = 40-bit PA */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("isb" ::: "memory");

    BOOT_EARLY_PUTC('4');  /* Step 4: TTBR0 set */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)hp232x_mmu_l1) : "memory");
    __asm__ volatile("isb" ::: "memory");

    BOOT_EARLY_PUTC('5');  /* Step 5: TLB invalidate + MMU enable */
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 0x1UL;             /* enable MMU */
    sctlr &= ~0x4UL;            /* disable data cache */
    sctlr &= ~0x1000UL;         /* disable instruction cache */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    /* Note: BOOT_EARLY_PUTC may hang after MMU enable due to UART state */
    /* Continue without printing marker 6 (optional) */
    volatile uint32_t *iram1_test = (volatile uint32_t *)IRAM1_USE_START;
    (void)*iram1_test;
    BOOT_EARLY_PUTC('7');  /* Step 7: IRAM1 test done */

    /* === CRITICAL: Full TLB + cache maintenance after MMU enable === */
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    /*
     * Never DC CIVAC VA=0: L2 maps VA0 as Device (Boot SSI/ROM).
     * Cache-maintaining Device memory is invalid and can SError.
     * Clean a Normal table line instead.
     */
    __asm__ volatile("dc civac, %0" : : "r"((uint64_t)hp232x_mmu_l1) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] Enable caches (D + I)");
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 0x4UL;             /* enable data cache */
    sctlr |= 0x1000UL;          /* enable instruction cache */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] HP232X MMU init complete (DMA NC 0x%llx +0x%llx)",
          (unsigned long long)IRAM1_DMA_NC_START,
          (unsigned long long)IRAM1_DMA_NC_SIZE);
#ifdef BSP_IRAM0_LOW_NC
    LOG_I("[mmu] IRAM0 low NC 0x%llx +0x%llx (BSP_IRAM0_LOW_NC)",
          (unsigned long long)IRAM0_LOW_START,
          (unsigned long long)IRAM0_LOW_SIZE);
#endif
#ifdef BSP_IRAM1_LOW_NC
    LOG_I("[mmu] IRAM1 host scratch NC 0x%llx +0x%llx (BSP_IRAM1_LOW_NC)",
          (unsigned long long)IRAM1_HOST_SCRATCH_START,
          (unsigned long long)IRAM1_HOST_SCRATCH_SIZE);
#else
    LOG_I("[mmu] IRAM1 host scratch WB 0x%llx +0x%llx (upgrade uses cache maintain)",
          (unsigned long long)IRAM1_HOST_SCRATCH_START,
          (unsigned long long)IRAM1_HOST_SCRATCH_SIZE);
#endif

    {
        uint64_t d0 = 0, d_alias = 0;
        uint32_t a0 = 0, a_alias = 0;
        int r0, r_alias;

        r0 = hp232x_mmu_check_l2_block(0x00000000UL, 0x00000000UL, &d0, &a0);
        r_alias = hp232x_mmu_check_l2_block(0x07000000UL, 0x00000000UL, &d_alias, &a_alias);
        LOG_I("[mmu] SSI map VA0→PA0 ret=%d desc=0x%llx attr=%u (expect attr=2 Device)",
              r0, (unsigned long long)d0, a0);
        LOG_I("[mmu] SSI alias VA0x07→PA0 ret=%d desc=0x%llx attr=%u",
              r_alias, (unsigned long long)d_alias, a_alias);
    }

    {
        uint32_t nc_idx = 0, wb_idx = 0;
        int vnc, vwb;

        if (hp232x_mmu_pte_attr_index(IRAM1_DMA_NC_START, &nc_idx) == 0 &&
            hp232x_mmu_pte_attr_index(IRAM1_BSS_START, &wb_idx) == 0)
        {
            LOG_I("[mmu] PTE attr: DMA NC va=0x%llx idx=%u, bss va=0x%llx idx=%u",
                  (unsigned long long)IRAM1_DMA_NC_START, nc_idx,
                  (unsigned long long)IRAM1_BSS_START, wb_idx);
        }

        vnc = hp232x_mmu_verify_dma_nc((uint64_t)(uintptr_t)__dma_nocache_start);
        vwb = hp232x_mmu_verify_dma_nc((uint64_t)IRAM1_BSS_START);
        LOG_I("[mmu] DMA NC verify interg=%d bss=%d (0=pass NC, bss expect fail)",
              vnc, vwb);
    }
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

    /* Silence: called under _cpus_lock on secondary — no LOG/kprintf. */
    (void)sctlr;
}