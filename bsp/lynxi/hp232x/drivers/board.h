/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 * 2025-06-17     lynxi          hp232x BSP, two-segment IRAM, direct boot
 */

#ifndef BOARD_H__
#define BOARD_H__

#include <stdint.h>
#include "lynxi.h"
#include "ioremap.h"

/*
 * HP232X / KA200 memory layout with constraints (Updated 2026-06-30):
 *
 * IRAM0 (512KB @ 0x04000000):
 *   BL21_BOOT mode:  FIRST 256KB (0x04000000-0x0403FFFF) USABLE
 *   BL22_BOOT mode:  LAST  256KB (0x04040000-0x0407FFFF) USABLE
 *   Other half is RESERVED for bootcode/SPL space.
 *
 * IRAM1 (512MB @ 0x100000000):
 *   FIRST  256KB (0x100000000-0x10003FFFF) RESERVED
 *   LAST   256KB (0x100040000-0x10007FFFF) USABLE
 *
 * No external DDR; the chip boots directly from bootcode in IRAM0
 * and jumps to rt-thread at the entry point.
 */

/* IRAM segment addresses — KA200 has 512KB per segment */
#define IRAM0_START     0x04000000UL
#define IRAM0_SIZE      0x00080000UL   /* 512 KB */
#define IRAM0_END       (IRAM0_START + IRAM0_SIZE)

/* Memory Constraint: IRAM0 usable region controlled by BL1_BOOT macro */
#ifdef BSP_USING_HP232X_BL21
/* BL21 mode: kernel at 0x04000020, bootcode jumps directly */
#define IRAM0_USE_START  0x04000000UL
#define IRAM0_RESERVED_START (IRAM0_USE_START + IRAM0_USE_SIZE)
#else
/* BL22 mode: kernel at 0x04040020, reserved first 256KB for bootwrapper+SPL */
#define IRAM0_USE_START  0x04040000UL
#define IRAM0_RESERVED_START 0x04000000UL
#endif
#define IRAM0_USE_SIZE   0x00040000UL   /* 256 KB - usable */
#define IRAM0_USE_END    (IRAM0_USE_START + IRAM0_USE_SIZE)
#define IRAM0_RESERVED_SIZE 0x00040000UL   /* 256 KB - reserved */

#define IRAM1_START     0x100000000ULL
#define IRAM1_SIZE      0x00080000UL   /* 512 KB */
#define IRAM1_END       (IRAM1_START + IRAM1_SIZE)

/* Memory Constraint: IRAM1 only use LAST 256KB */
#define IRAM1_RESERVED_START IRAM1_START
#define IRAM1_RESERVED_SIZE 0x00040000ULL   /* 256 KB - reserved (low half) */
#define IRAM1_RESERVED_END (IRAM1_RESERVED_START + IRAM1_RESERVED_SIZE)
#define IRAM1_USE_START IRAM1_RESERVED_END  /* 0x100040000 - usable start */
#define IRAM1_USE_SIZE  0x00040000UL   /* 256 KB - usable */
#define IRAM1_USE_END   (IRAM1_USE_START + IRAM1_USE_SIZE)

/* IRAM1 detailed layout within usable 256KB (0x100040000-0x10007FFFF) */
/* Unified stack area: 0x100069000-0x10007FFF0 (92KB) — grows DOWN from top */
/* Other segments unchanged: BSS, Page pool, Heap */
#define IRAM1_BSS_START        0x100050000ULL  /* .bss section start (52KB) */
#define IRAM1_PAGE_START       0x10005D000ULL  /* Page pool start (16KB) */
#define IRAM1_HEAP_START       0x100061000ULL  /* Heap start (32KB) */
#define IRAM1_HEAP_END         0x100069000ULL  /* Heap end = Stack bottom */
#define IRAM1_STACK_BOTTOM     0x100069000ULL  /* Stack area starts here */
#define IRAM1_STACK_TOP        0x10007FFF0ULL  /* Stack top (92KB, grows DOWN) */

/* Mailbox for secondary CPU spin-table — in IRAM0 (bootwrapper mbox_address=0x401ff00) */
#define MBOX_ADDRESS         0x0401FF00ULL    /* bootwrapper mbox in IRAM0, NOT IRAM1 */

/* Kernel text/data in IRAM0 first 256KB, runtime data in IRAM1 last 256KB */
#define MEM_PADDR_START IRAM1_USE_START  /* 0x100040000 */
#define MEM_CACHE_SZ    (IRAM1_USE_END - IRAM1_BSS_START)  /* Space after .bss */
#define MEM_NOCACHE_SZ  0             /* no nocache region in pure-IRAM config */

extern int __bss_end;

/* Heap and page pool sizes — must fit within IRAM1 usable 256KB */
#define PAGE_POOL_SIZE          0x4000UL    /* 16KB */
#define HEAP_POOL_SIZE          0x08000UL    /* 32KB */

/* Kernel text/data base address — depends on BL2_BOOT macro */
#ifdef BSP_USING_HP232X_BL21
#define KERNEL_VADDR_START     0x04000020UL
#define IRAM0_KERNEL_BASE      0x04000000UL
#else
#define KERNEL_VADDR_START     0x04040020UL
#define IRAM0_KERNEL_BASE      0x04040000UL
#endif

void rt_hw_board_init(void);

#endif
