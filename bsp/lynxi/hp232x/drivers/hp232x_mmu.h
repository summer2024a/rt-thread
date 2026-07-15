#ifndef __HP232X_MMU_H__
#define __HP232X_MMU_H__

#include <stdint.h>

#ifdef __has_include
#  if __has_include_next(<mmu.h>)
#    include_next <mmu.h>
#  else
#    error "Cannot locate upstream mmu.h"
#  endif
#else
#  include_next <mmu.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HP232X_MMU_ENTRIES          512
#define HP232X_MMU_PAGE_SIZE        0x1000UL
#define HP232X_MMU_L1_INDEX_RAM1    4

/*
 * APU address map (lyn_apu v2 / lynchip-lite EVB, SLV0):
 *   SLV0 + NORM     0x1400000000  CR/MDBG/cmd (APU_BASE_NORM_SIZE ~6MB)
 *   + core tile     0x1511800134~0x156B800134  apu_hw_init fix reg
 *   SLV0 NN FIFO    0x1600000000~0x1670000000  load/start inference
 * CPR @ 0x12500000 is in L2_low, not here.
 *
 * Each 1GB slot uses one L1 block (no extra L2/L3 tables).
 */
#define HP232X_APU_NORM_BASE        0x1400000000UL
#define HP232X_APU_CORECFG0_BASE    0x1500000000UL
#define HP232X_APU_CORECFG1_BASE    0x1540000000UL
#define HP232X_APU_NN_FIFO0_BASE    0x1600000000UL
#define HP232X_APU_NN_FIFO1_BASE    0x1640000000UL

#define HP232X_APU_BASE             HP232X_APU_NORM_BASE
#define HP232X_MMU_L1_INDEX_APU     (HP232X_APU_NORM_BASE >> 30)

#define DESC_VALID                  (1UL << 0)
#define DESC_TABLE                  (DESC_VALID | (1UL << 1))
#define DESC_BLOCK                  DESC_VALID
#define DESC_PAGE                   DESC_TABLE

#define ATTR_INDEX_NORMAL_WB        (0UL << 2)
#define ATTR_INDEX_NORMAL_NC        (1UL << 2)
#define ATTR_INDEX_DEVICE           (2UL << 2)

#define DESC_AP_RW_EL1              (0UL << 6)
#define DESC_SH_OUTER               (2UL << 8)
#define DESC_SH_INNER               (3UL << 8)
#define DESC_AF                     (1UL << 10)
#define DESC_PXN                    (1UL << 53)
#define DESC_UXN                    (1UL << 54)

#define ATTR_NORMAL_WB              (ATTR_INDEX_NORMAL_WB | DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF)
#define ATTR_NORMAL_NC              (ATTR_INDEX_NORMAL_NC | DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF)
#define ATTR_DEVICE                 (ATTR_INDEX_DEVICE | DESC_AP_RW_EL1 | DESC_SH_OUTER | DESC_AF | DESC_PXN | DESC_UXN)

#define ADDR_MASK_TABLE             0x0000fffffffff000UL
#define ADDR_MASK_L1_BLOCK          0x0000fffffc000000UL
#define ADDR_MASK_L2_BLOCK          0x0000ffffffe00000UL
#define ADDR_MASK_L3_PAGE           0x0000fffffffff000UL

void hp232x_mmu_init(void);

/**
 * Flush all HP232X page tables to PoC (call after modifying entries in cacheable RAM).
 */
void hp232x_mmu_flush_tables(void);

/**
 * Verify VA is an L2 BLOCK mapping to expect_pa (2MB-aligned compare).
 * Returns 0 on match. out_desc / out_attr_idx optional.
 */
int hp232x_mmu_check_l2_block(uint64_t va, uint64_t expect_pa,
                              uint64_t *out_desc, uint32_t *out_attr_idx);

/**
 * Verify VA is mapped ATTR_NORMAL_NC in software page tables.
 * Returns 0 if AttrIndx==1 (NC), negative on mismatch.
 */
int hp232x_mmu_pte_attr_index(uint64_t va, uint32_t *out_attr_idx);

/**
 * Runtime DMA-NC sanity check for @test_va (store/load + PTE attr).
 * Returns 0 on pass.
 */
int hp232x_mmu_verify_dma_nc(uint64_t test_va);

/**
 * Get the L1 page table base address for SMP secondary CPU.
 *
 * In SMP mode, all CPUs share the same page table (hp232x_mmu_l1).
 * Secondary CPU needs to call this function to get the page table
 * address before calling rt_hw_mmu_ktbl_set().
 *
 * @return Physical address of the L1 page table
 */
uint64_t *hp232x_mmu_get_l1_table(void);

/**
 * Initialize MMU for secondary CPU in SMP mode.
 *
 * This function sets up the shared page table (hp232x_mmu_l1),
 * flushes TLB, and ensures MMU is enabled. It replaces the
 * standard rt_hw_mmu_ktbl_set() for HP232X BSP.
 *
 * Called from rt_hw_secondary_cpu_bsp_start() in board.c.
 *
 * NOTE: This function must be called with MMU either already enabled
 * (by bootwrapper) or disabled. It handles both cases.
 */
void hp232x_mmu_secondary_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __HP232X_MMU_H__ */