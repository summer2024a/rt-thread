/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2017-08-24     chinesebear  first version
 */


#include "synopGMAC_plat.h"
#include "synopGMAC_Dev.h"
#include <rthw.h>
#include <rtthread.h>
#include <mm_aspace.h>
#include <mmu.h>

void flush_cache(unsigned long start_addr, unsigned long size)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)start_addr, (int)size);
}

/*
 * HE200 is AArch64 with DDR in high physical addresses. Legacy MIPS-style
 * CACHED_TO_UNCACHED / VA_TO_PA macros in synopGMAC_plat.h map heap VAs to
 * 0xa0xxxxxx and truncate PA — that window is not mapped and causes faults
 * when initializing DMA descriptors. Use kernel VA->PA from the MMU.
 */
dma_addr_t gmac_dmamap(unsigned long va, u32 size)
{
    void *pa;

    (void)size;
    pa = rt_kmem_v2p((void *)va);
    if (pa == RT_NULL || pa == ARCH_MAP_FAILED)
    {
        return (dma_addr_t)0;
    }
    return (dma_addr_t)(rt_ubase_t)pa;
}



/**
  * This is a wrapper function for Memory allocation routine. In linux Kernel
  * it it kmalloc function
  * @param[in] bytes in bytes to allocate
  */

void *plat_alloc_memory(u32 bytes)
{
//return (void*)malloc((size_t)bytes, M_DEVBUF, M_DONTWAIT);
    void *buf = (void*)rt_malloc((u32)bytes);

    flush_cache((unsigned long)buf, bytes);
    return buf;
}

/**
  * This is a wrapper function for consistent dma-able Memory allocation routine.
  * In linux Kernel, it depends on pci dev structure
  * @param[in] bytes in bytes to allocate
  */

//void *plat_alloc_consistent_dmaable_memory(struct synopGMACdevice *dev, u32 size, u32 *addr)
void *plat_alloc_consistent_dmaable_memory(synopGMACdevice *pcidev, u32 size, u32 *addr)
{
    rt_ubase_t raw;
    rt_ubase_t aligned;
    void *pa;
    u32 total;

    (void)pcidev;
    total = size + (u32)sizeof(void *) + 16U;
    raw = (rt_ubase_t)rt_malloc(total);
    if (raw == 0U)
    {
        return RT_NULL;
    }

    aligned = (raw + (rt_ubase_t)sizeof(void *) + 15UL) & ~(rt_ubase_t)15UL;
    *(void **)(aligned - sizeof(void *)) = (void *)raw;

    pa = rt_kmem_v2p((void *)aligned);
    if (pa == RT_NULL || pa == ARCH_MAP_FAILED)
    {
        rt_free((void *)raw);
        return RT_NULL;
    }

    flush_cache((unsigned long)aligned, size);
    *addr = (u32)(rt_ubase_t)pa;
    return (void *)aligned;
}


/**
  * This is a wrapper function for freeing consistent dma-able Memory.
  * In linux Kernel, it depends on pci dev structure
  * @param[in] bytes in bytes to allocate
  */


//void plat_free_consistent_dmaable_memory(void * addr)
void plat_free_consistent_dmaable_memory(synopGMACdevice *pcidev, u32 size, void * addr,u32 dma_addr)
{
    void *raw;

    (void)pcidev;
    (void)size;
    (void)dma_addr;
    if (addr == RT_NULL)
    {
        return;
    }
    raw = *(void **)((char *)addr - sizeof(void *));
    rt_free(raw);
}



/**
  * This is a wrapper function for Memory free routine. In linux Kernel
  * it it kfree function
  * @param[in] buffer pointer to be freed
  */
void plat_free_memory(void *buffer)
{
    rt_free(buffer);
    return ;
}



dma_addr_t plat_dma_map_single(void *hwdev, void *ptr,
                            u32 size)
{
        unsigned long addr = (unsigned long) ptr;
//CPU_IOFlushDCache(addr,size, direction);
    flush_cache(addr, size);
return gmac_dmamap(addr, size);
}

/**
  * This is a wrapper function for platform dependent delay
  * Take care while passing the argument to this function
  * @param[in] buffer pointer to be freed
  */
void plat_delay(u32 delay)
{
    while (delay--);
    return;
}


