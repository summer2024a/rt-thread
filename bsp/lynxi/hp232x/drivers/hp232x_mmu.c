#define DBG_TAG "hp232x_mmu"
#define DBG_LVL DBG_LOG

#include <rtdbg.h>
#include <rtthread.h>
#include <stdint.h>

#include "board.h"
#include "hp232x_mmu.h"

static uint64_t hp232x_mmu_l1[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".mmu_table")));
static uint64_t hp232x_mmu_l2_low[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".mmu_table")));
static uint64_t hp232x_mmu_l2_ram1[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".mmu_table")));
static uint64_t hp232x_mmu_l2_apu[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".mmu_table")));
static uint64_t hp232x_mmu_l3_ram0[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".mmu_table")));
static uint64_t hp232x_mmu_l3_ram1[HP232X_MMU_ENTRIES]
    __attribute__((aligned(4096), section(".mmu_table")));

static inline uint64_t hp232x_desc_table(uint64_t addr)
{
    return (addr & ADDR_MASK_TABLE) | DESC_TABLE;
}

static inline uint64_t hp232x_desc_block(uint64_t addr, uint64_t attr)
{
    return (addr & ADDR_MASK_L2_BLOCK) | attr | DESC_BLOCK;
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

static void hp232x_build_page_tables(void)
{
    rt_memset(hp232x_mmu_l1, 0, sizeof(hp232x_mmu_l1));
    rt_memset(hp232x_mmu_l2_low, 0, sizeof(hp232x_mmu_l2_low));
    rt_memset(hp232x_mmu_l2_ram1, 0, sizeof(hp232x_mmu_l2_ram1));
    rt_memset(hp232x_mmu_l2_apu, 0, sizeof(hp232x_mmu_l2_apu));
    rt_memset(hp232x_mmu_l3_ram0, 0, sizeof(hp232x_mmu_l3_ram0));
    rt_memset(hp232x_mmu_l3_ram1, 0, sizeof(hp232x_mmu_l3_ram1));

    hp232x_mmu_l1[0] = hp232x_desc_table((uint64_t)hp232x_mmu_l2_low);
    hp232x_mmu_l1[HP232X_MMU_L1_INDEX_RAM1] = hp232x_desc_table((uint64_t)hp232x_mmu_l2_ram1);
    hp232x_mmu_l1[HP232X_MMU_L1_INDEX_APU] = hp232x_desc_table((uint64_t)hp232x_mmu_l2_apu);

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

    hp232x_mmu_l2_apu[0] = hp232x_desc_block(0x1000000000UL, ATTR_DEVICE);
}

void hp232x_mmu_init(void)
{
    uint64_t tcr;
    uint64_t sctlr;
    uint64_t mair = 0x000044ffUL;

    LOG_I("[mmu] Build HP232X page tables in IRAM0");
    hp232x_build_page_tables();

    LOG_I("[mmu] Configure MAIR_EL1");
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] Configure TCR_EL1");
    tcr = 25UL;                 /* T0SZ = 25 (39-bit VA) */
    tcr |= (1UL << 8);          /* IRGN0 = WB WA RA */
    tcr |= (1UL << 10);         /* ORGN0 = WB WA RA */
    tcr |= (3UL << 12);         /* SH0 = Inner Shareable */
    tcr |= (2UL << 32);         /* IPS = 40-bit PA */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr) : "memory");
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] Set TTBR0_EL1 to L1 table at %p", (void *)hp232x_mmu_l1);
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)hp232x_mmu_l1) : "memory");
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] Invalidate TLBs and enable MMU");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 0x1UL;             /* enable MMU */
    sctlr &= ~0x4UL;            /* disable data cache */
    sctlr &= ~0x1000UL;         /* disable instruction cache */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] MMU enabled, verifying IRAM1 access");
    volatile uint32_t *iram1_test = (volatile uint32_t *)IRAM1_USE_START;
    (void)*iram1_test;
    LOG_I("[mmu] IRAM1 test read 0x%08x from 0x%llx", *iram1_test, (unsigned long long)IRAM1_USE_START);

    LOG_I("[mmu] Enable caches");
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 0x4UL;             /* enable data cache */
    sctlr |= 0x1000UL;          /* enable instruction cache */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");

    LOG_I("[mmu] HP232X MMU init complete");
}
