/*
 * Copyright (c) 2024, RT-Thread Development Team
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "platform/rpmsg_platform.h"

#include <rtthread.h>
#include <rthw.h>
#include <string.h>

#define DBG_TAG "rpmsg-platform"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#include "virtqueue.h"

/* Timer definitions from the Lynxi BSP */
#include "drivers/drv_timer.h"
#include "lynxi.h"

/* PCIe MSI-X definitions from the Lynxi BSP */
#include "drivers/pcie/ep/drv_pcie.h"
#include <stdio.h>

/*
 * RPMsg slot = buffer_payload_size + sizeof(rpmsg_std_hdr); payload must be (2^n - 16).
 * buffer_count must be a power of two. Keep in sync with Linux
 * tools/drivers_test/rpmsg-net/rpmsg_net_bridge.c (same BAR IPC region).
 *
 * Shared memory layout (rpmsg_lite master init):
 *   [0, vring_size)           = rx vring
 *   [vring_size, 2*vring_size) = tx vring
 *   [2*vring_size, ...)       = data buffers (slot = payload + 16 each)
 * vring_size must be >= virtio_ring.h vring_size(buffer_count, vring_align) for each ring,
 * or rings overlap each other or the buffer pool.
 */
#define RPMSG_PLATFORM_BUFFER_PAYLOAD_SIZE  4080U  /* 4096-byte slot */
#define RPMSG_PLATFORM_BUFFER_COUNT         128U
#define RPMSG_PLATFORM_VRING_SIZE           16384U

#if defined(RT_USING_SMP) && defined(BSP_RPMSG_NET_BIND_CPU0)
#include <interrupt.h>
#endif

/* Default timer index used for Host->Device notification */
#define RPMSG_PLATFORM_TIMER_TVQ_IDX 2
#define RPMSG_PLATFORM_TIMER_RVQ_IDX 3

/* Base MSI-X vector ID for RPMsg-Lite interrupts */
#define LYND_PCI_MSIX_TVQ_NO 4
#define LYND_PCI_MSIX_RVQ_NO 5

static rpmsg_platform_role_t g_platform_role = RPMSG_PLATFORM_ROLE_REMOTE;

struct timer_config_t {
    uint32_t index;
    uint32_t irq_no;
    char name[16];
    timer_handle_t handle;
    void *private_data;
};

static struct timer_config_t g_timer_config[2] = {
    {RPMSG_PLATFORM_TIMER_TVQ_IDX, TIMER_IRQ_START + RPMSG_PLATFORM_TIMER_TVQ_IDX, "rpn_tx", NULL, NULL},
    {RPMSG_PLATFORM_TIMER_RVQ_IDX, TIMER_IRQ_START + RPMSG_PLATFORM_TIMER_RVQ_IDX, "rpn_rx", NULL, NULL}
};

/* Internal helper: call virtqueue callback on interrupt */
static void rpmsg_platform_vq_isr(void *param)
{
    struct virtqueue *vq = (struct virtqueue *)param;

    if (vq && vq->vq_name)
        LOG_D("vq %s, ISR received", vq->vq_name);
    else
        LOG_E("vq NULL, ISR received");

    virtqueue_notification(vq);
}

static void rpmsg_platform_timer_cb(timer_event_e event, void *arg)
{
    if (event == TIMER_EVENT_TIMEOUT)
    {
        struct timer_config_t *timer = (struct timer_config_t *)arg;

        LOG_D("Timer(%p) %d, %s cb received", timer, timer->index, timer->name);
        rpmsg_platform_vq_isr(timer->private_data);
    }
}

static void rpmsg_platform_timer_irq(int vector, void *param)
{
    struct timer_config_t *timer = (struct timer_config_t *)param;

    LOG_D("Timer(%p) %d, %s IRQ %d received", param, timer->index, timer->name, vector);

    /* Clear the hardware interrupt and run the timer callback. */
    rpmsg_timer_irqhandler(timer->handle);
}

#if defined(RT_USING_SMP) && defined(BSP_RPMSG_NET_BIND_CPU0)

#ifndef BSP_RPMSG_NET_CPU
#define BSP_RPMSG_NET_CPU 0
#endif
#ifndef BSP_RPMSG_NET_IRQ_CPU
#define BSP_RPMSG_NET_IRQ_CPU 0
#endif

static int rpmsg_platform_irq_cpu_clamp(int cpu)
{
    if (cpu < 0)
    {
        cpu = 0;
    }
#if defined(RT_CPUS_NR)
    if (cpu >= (int)RT_CPUS_NR)
    {
        cpu = (int)RT_CPUS_NR - 1;
    }
#endif
    return cpu;
}

static int rpmsg_platform_timer_irq_cpu_index(void)
{
    int cpu;

#if defined(BSP_RPMSG_NET_IRQ_FOLLOW_WORKER_CPU)
    cpu = BSP_RPMSG_NET_CPU;
#elif defined(BSP_RPMSG_NET_IRQ_CPU)
    cpu = BSP_RPMSG_NET_IRQ_CPU;
#else
    /* Legacy rtconfig: no IRQ options, match workers. */
    cpu = BSP_RPMSG_NET_CPU;
#endif
    return rpmsg_platform_irq_cpu_clamp(cpu);
}
#endif

static void rpmsg_platform_timer_init(void)
{
    for (int i = 0; i < 2; i++)
    {
        timer_handle_t handle = dw_timer_initialize(g_timer_config[i].index,
            rpmsg_platform_timer_cb, &g_timer_config[i]);
        rpmsg_timer_prepare(handle);
        g_timer_config[i].handle = handle;
    }
}

static int rpmsg_platform_timer_request_irq(int index, void *arg)
{
    struct timer_config_t *timer = NULL;
    int irq_no;

    if (index < 0 || index >= 2)
    {
        return -EINVAL;
    }

    timer = &g_timer_config[index];
    timer->private_data = arg;
    irq_no = (int)timer->irq_no;
    LOG_D("Timer(%p) %d, %s IRQ %d installed", timer, timer->index, timer->name, irq_no);
    rt_hw_interrupt_install(irq_no, (rt_isr_handler_t)rpmsg_platform_timer_irq,
                            (void *)timer, timer->name);
    rt_hw_interrupt_umask(irq_no);
#if defined(RT_USING_SMP) && defined(BSP_RPMSG_NET_BIND_CPU0)
    {
        int cpu = rpmsg_platform_timer_irq_cpu_index();

        if (rt_hw_interrupt_set_affinity(irq_no, cpu) != RT_EOK)
        {
            LOG_W("timer irq %d affinity -> cpu %d failed", irq_no, cpu);
        }
    }
#endif

   return 0;
}

static void rpmsg_platform_timer_deinit(void)
{
    for (int i = 0; i < 2; i++)
    {
        dw_timer_uninitialize(g_timer_config[i].handle);
    }
}

int32_t platform_get_custom_shmem_config(uint32_t link_id, rpmsg_platform_shmem_config_t *shmem_config)
{
    (void)link_id;

    if (shmem_config == RT_NULL)
    {
        return -1;
    }

    /* Configure based on expected shared memory layout (must match Linux host). */
    shmem_config->buffer_payload_size = RPMSG_PLATFORM_BUFFER_PAYLOAD_SIZE;
    shmem_config->buffer_count        = RPMSG_PLATFORM_BUFFER_COUNT;
    shmem_config->vring_size          = RPMSG_PLATFORM_VRING_SIZE;
    shmem_config->vring_align         = 4096U;

    LOG_D("Custom shmem config: payload_size=%u, count=%u, vring_size=%u, vring_align=%u",
          shmem_config->buffer_payload_size, shmem_config->buffer_count,
          shmem_config->vring_size, shmem_config->vring_align);

    return 0;
}

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data)
{
    struct virtqueue *vq = (struct virtqueue *)isr_data;

    if (vq == RT_NULL)
    {
        return -EINVAL;
    }

    /* Only the "rx" virtqueue needs a local interrupt handler (from host->device). */
    if (strncmp(vq->vq_name, "rx_vq", sizeof(vq->vq_name)) == 0) {
        rt_pci_msix_register_vector(LYND_PCI_MSIX_RVQ_NO);
        rpmsg_platform_timer_request_irq(1, isr_data);
    } else if (strncmp(vq->vq_name, "tx_vq", sizeof(vq->vq_name)) == 0) {
        rt_pci_msix_register_vector(LYND_PCI_MSIX_TVQ_NO);
        rpmsg_platform_timer_request_irq(0, isr_data);
    }

    return 0;
}

int32_t platform_deinit_interrupt(uint32_t vector_id)
{
    int vector = (vector_id & 0x1) ? LYND_PCI_MSIX_RVQ_NO : LYND_PCI_MSIX_TVQ_NO;

    return rt_pci_one_msix_deinit(vector);
}

#if defined(RL_USE_ENVIRONMENT_CONTEXT) && (RL_USE_ENVIRONMENT_CONTEXT == 1)
int32_t platform_notify(void *platform_context, uint32_t vector_id)
#else
int32_t platform_notify(uint32_t vector_id)
#endif
{
    int vector = (vector_id & 0x1) ? LYND_PCI_MSIX_RVQ_NO : LYND_PCI_MSIX_TVQ_NO;

    if (g_platform_role == RPMSG_PLATFORM_ROLE_REMOTE)
    {
        return rt_pci_msix_raise_irq(vector);
    }

    /* Master (host) notifies Remote via timer interrupt.
     * The host side typically writes to the timer compare register.
     * Here we just trigger the IRQ locally so the device sees it.
     */
    /* rt_hw_interrupt_trigger is not implemented in RT-Thread */
    return 0;
}

void platform_set_role(rpmsg_platform_role_t role)
{
    g_platform_role = role;
}

int32_t platform_in_isr(void)
{
    return 0;
}

int32_t platform_init(void)
{
    /* Initialize MSI-X interrupts. */
    rt_pci_msix_init();

    /* Timer interrupt is used for Remote (device) to notify Host (master) of data availability. */
    rpmsg_platform_timer_init();

    return 0;
}

int32_t platform_deinit(void)
{
    rt_pci_msix_deinit();
    rpmsg_platform_timer_deinit();

    return 0;
}

uintptr_t platform_vatopa(void *address)
{
    return (uintptr_t)address;
}

void *platform_patova(uintptr_t address)
{
    return (void *)(uintptr_t)address;
}

int32_t platform_interrupt_enable(uint32_t vector_id)
{
    (void)vector_id;
    return 0;
}

int32_t platform_interrupt_disable(uint32_t vector_id)
{
    (void)vector_id;
    return 0;
}

void platform_map_mem_region(uint32_t va, uint32_t pa, uint32_t size, uint32_t flags)
{
    (void)va;
    (void)pa;
    (void)size;
    (void)flags;
}

void platform_cache_all_flush_invalidate(void)
{
}

void platform_cache_disable(void)
{
}