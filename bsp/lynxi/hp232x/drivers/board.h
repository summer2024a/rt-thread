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
#include "mmu.h"
#include "ioremap.h"

/*
 * HP232X / KA200 memory layout with constraints (Updated 2026-06-23):
 *
 * Memory Constraints:
 *   IRAM0 (512KB @ 0x04000000): Only use FIRST 256KB (0x04000000-0x0403FFFF)
 *   IRAM0 (512KB @ 0x04000000): LAST 256KB (0x04040000-0x0407FFFF) RESERVED
 *   IRAM1 (512KB @ 0x100000000): FIRST 256KB (0x100000000-0x10003FFFF) RESERVED
 *   IRAM1 (512KB @ 0x100000000): Only use LAST 256KB (0x100040000-0x10007FFFF)
 *
 * IRAM0 Usage (0x04000000-0x0403FFFF, 256KB):
 *   0x04000000-0x0400001F: PCIe Boot Header (32B)
 *   0x04000020-...: kernel text + rodata + data + mmu_table (~240KB)
 *
 * IRAM1 Usage (0x100040000-0x10007FFFF, 256KB):
 *   0x100040000-0x10004FFFF: CPU stacks + early data (~64KB)
 *   0x100050000-...: .bss section (linked)
 *   page pool: 16KB (after .bss)
 *   heap: 48KB (after page pool)
 *   stack top: 0x10007FFFC (grows downward)
 *
 * No external DDR; the chip boots directly from bootcode in IRAM0
 * and jumps to rt-thread at the entry point.
 */

/* IRAM segment addresses — KA200 has 512KB per segment */
#define IRAM0_START     0x04000000UL
#define IRAM0_SIZE      0x00080000UL   /* 512 KB */
#define IRAM0_END       (IRAM0_START + IRAM0_SIZE)

/* Memory Constraint: IRAM0 only use FIRST 256KB */
#define IRAM0_USE_START 0x04000000UL
#define IRAM0_USE_SIZE  0x00040000UL   /* 256 KB - usable */
#define IRAM0_USE_END   (IRAM0_USE_START + IRAM0_USE_SIZE)
#define IRAM0_RESERVED_START IRAM0_USE_END
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
#define IRAM1_STACK_AREA_START 0x100040000ULL  /* CPU stacks + early data (~64KB) */
#define IRAM1_BSS_START        0x100050000ULL  /* .bss section start */

/* Stack configuration — MUST be in IRAM1 high 256KB (after 0x100040000) */
#define IRAM1_STACK_TOP     0x10007FFFCULL   /* Stack top at IRAM1 end - 4 bytes, grows downward */

/* Mailbox for secondary CPU spin-table — in IRAM0 (bootwrapper mbox_address=0x401ff00) */
#define MBOX_ADDRESS         0x0401FF00ULL    /* bootwrapper mbox in IRAM0, NOT IRAM1 */

/* Kernel text/data in IRAM0 first 256KB, runtime data in IRAM1 last 256KB */
#define MEM_PADDR_START IRAM1_USE_START  /* 0x100040000 */
#define MEM_CACHE_SZ    (IRAM1_USE_END - IRAM1_BSS_START)  /* Space after .bss */
#define MEM_NOCACHE_SZ  0             /* no nocache region in pure-IRAM config */

extern int __bss_end;

#define PAGE_POOL_SIZE          0x4000UL    /* 16KB */
#define HEAP_POOL_SIZE          0xC000UL    /* 48KB - working value */

#define KERNEL_VADDR_START 0

void rt_hw_board_init(void);

#endif
