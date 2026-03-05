/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 */

#ifndef BOARD_H__
#define BOARD_H__

#include <stdint.h>
#include "lynxi.h"
#include "mmu.h"
#include "ioremap.h"

extern int __bss_end;
#define HEAP_BEGIN      ((void*)&__bss_end)

#ifdef RT_USING_SMART
#define HEAP_END        ((size_t)KERNEL_VADDR_START + 32 * 1024 * 1024)
#define PAGE_START      HEAP_END
#define PAGE_END        ((size_t)KERNEL_VADDR_START + 128 * 1024 * 1024)
#else

#define MEM_PADDR_START 0x800000000
#define MEM_NOCACHE_SZ  0x40000000
#define MEM_CACHE_SZ    0x40000000

#ifndef RT_USING_SMART
#define KERNEL_VADDR_START 0
#endif

#define HEAP_END        (MEM_PADDR_START + 32 * 1024 * 1024)
#define PAGE_START      HEAP_END
#define PAGE_END        ((size_t)PAGE_START + 128 * 1024 * 1024)
#endif

void rt_hw_board_init(void);

#endif
