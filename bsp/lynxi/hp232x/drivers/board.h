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

#include <stddef.h>
#include <stdint.h>
#include <rtconfig.h>
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

/*
 * Dedicated non-cacheable DMA arena at IRAM1 usable low 64KB (hp640 CONFIG_SPL_BSS).
 * MMU maps this NC; eMMC/ADMA descriptors and bounce buffers live here so SDHCI
 * sees CPU writes without per-xfer dcache flush. Rest of IRAM1 stays WB.
 */
#define IRAM1_DMA_NC_START      IRAM1_USE_START
#define IRAM1_DMA_NC_SIZE       0x00010000UL   /* 64 KB */
#define IRAM1_DMA_NC_END        (IRAM1_DMA_NC_START + IRAM1_DMA_NC_SIZE)

/*
 * Host upgrade scratch (= IRAM1 reserved low 256KB / ka200 DDR_IRAM_ADDR).
 * When BSP_IRAM1_LOW_NC: MMU maps this range ATTR_NORMAL_NC.
 */
#define IRAM1_HOST_SCRATCH_START IRAM1_RESERVED_START
#define IRAM1_HOST_SCRATCH_SIZE  IRAM1_RESERVED_SIZE
#define IRAM1_HOST_SCRATCH_END   IRAM1_RESERVED_END

/*
 * IRAM0 low 256KB — Host biz descriptors / data (BL22; boot-wrapper half).
 * When BSP_IRAM0_LOW_NC: MMU maps ATTR_NORMAL_NC (mbox @ 0x401fff0 included).
 */
#define IRAM0_LOW_START   IRAM0_START
#define IRAM0_LOW_SIZE    0x00040000UL
#define IRAM0_LOW_END     (IRAM0_LOW_START + IRAM0_LOW_SIZE)

/* True if [addr, addr+size) lies entirely in a Normal-NC IRAM window. */
static inline int hp232x_addr_is_normal_nc(const void *addr, size_t size)
{
    uintptr_t start;
    uintptr_t end;

    if (!addr || size == 0)
        return 0;

    start = (uintptr_t)addr;
    end = start + size;
    if (end < start)
        return 0;

#ifndef BSP_EMMC_DMA_CACHED_BSS
    if (start >= (uintptr_t)IRAM1_DMA_NC_START && end <= (uintptr_t)IRAM1_DMA_NC_END)
        return 1;
#endif
#ifdef BSP_IRAM1_LOW_NC
    if (start >= (uintptr_t)IRAM1_HOST_SCRATCH_START &&
        end <= (uintptr_t)IRAM1_HOST_SCRATCH_END)
        return 1;
#endif
#ifdef BSP_IRAM0_LOW_NC
    if (start >= (uintptr_t)IRAM0_LOW_START && end <= (uintptr_t)IRAM0_LOW_END)
        return 1;
#endif
    return 0;
}

/*
 * Both IRAM0+IRAM1 low-256KB mapped NC: biz hot path omits Host dcache ops
 * at compile time (see #ifndef BSP_BIZ_SKIP_HOST_DCACHE).
 */
#if defined(BSP_IRAM0_LOW_NC) && defined(BSP_IRAM1_LOW_NC)
#define BSP_BIZ_SKIP_HOST_DCACHE
#endif

/* IRAM1 detailed layout within usable 256KB (0x100040000-0x10007FFFF) */
/* DMA NC arena: 0x100040000-0x100050000 (64KB) — .dma_nocache in link.lds */
#define IRAM1_BSS_START        0x100050000ULL  /* .bss section start (52KB) */
#define BIZ_LOG_BUFFER_ADDR    IRAM1_BSS_START /* persistent log ring @ warm reset */
#define IRAM1_PAGE_START       0x10005D000ULL  /* Page pool start (16KB) */
#define IRAM1_HEAP_START       0x100061000ULL  /* Heap start (32KB) */
#define IRAM1_HEAP_END         0x100069000ULL  /* Heap end = Stack bottom */
#define IRAM1_STACK_BOTTOM     0x100069000ULL  /* Stack area starts here */
#define IRAM1_STACK_TOP        0x10007FFF0ULL  /* Stack top (92KB, grows DOWN) */

/*
 * Secondary spin-table mailbox — IRAM0, aligns bootwrapper mbox_address.
 * Board/git: 0x401fff0 (7 secondary slots, stride 8; was wrongly 0x401ff00).
 * spin.S: addr = mbox_address + (linear_id - 1) * 8; CPU1 → +0.
 */
#define MBOX_ADDRESS         0x0401FFF0ULL

#define HP232X_CPU_RELEASE_MBOX(cpu_id) \
    (MBOX_ADDRESS + (unsigned long)(((cpu_id) - 1ULL) * 8ULL))

/* Kernel text/data in IRAM0 first 256KB, runtime data in IRAM1 last 256KB */
#define MEM_PADDR_START IRAM1_USE_START  /* 0x100040000 */
#define MEM_CACHE_SZ    (IRAM1_USE_END - IRAM1_BSS_START)  /* Space after .bss */
#define MEM_NOCACHE_SZ  IRAM1_DMA_NC_SIZE

extern char __dma_nocache_start[];
extern char __dma_nocache_end[];
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

void early_putc_direct(char c);

/* Primary-CPU boot breadcrumbs — see BSP_BOOT_EARLY_MARK in rtconfig.h */
#ifdef BSP_BOOT_EARLY_MARK
#define BOOT_EARLY_PUTC(c) early_putc_direct(c)
#else
#define BOOT_EARLY_PUTC(c) ((void)0)
#endif

#if defined(RT_USING_SMP) && defined(BSP_USING_HP232X)
/* After bind+startup to CPU1: flush rt_cpu + SCHEDULE IPI + SEV (see doc/SMP_SETUP.md). */
void hp232x_kick_cpu(int cpu);
void hp232x_smp_release_cpu(int cpu);
int hp232x_smp_wait_secondaries(void);
#endif

#ifdef BSP_BIZ_HOTPATH_NO_TICK_IPI
/* Call on emmc_biz CPU: disable local tick + mask schedule/stop/smp_call SGIs. */
void hp232x_biz_hotpath_irq_quiet(void);
#endif

#endif
