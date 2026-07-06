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
#define HP232X_MMU_L1_INDEX_APU     64

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
#define ADDR_MASK_L2_BLOCK          0x0000ffffffe00000UL
#define ADDR_MASK_L3_PAGE           0x0000fffffffff000UL

void hp232x_mmu_init(void);

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
