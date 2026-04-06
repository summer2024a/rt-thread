/*
 * Copyright (c) 2024, RT-Thread Development Team
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RPMSG_PLATFORM_H_
#define RPMSG_PLATFORM_H_

#include <stdint.h>

/*
 * RPMsg-Lite platform configuration for RT-Thread on KA200 (Lynxi HE200).
 *
 * Shared memory for RPMsg-Lite is located in PCIe BAR and is reserved by the
 * host using IB rules.
 */

/* Shared memory region used by RPMsg-Lite (must match Host configuration). */
// #define RPMSG_LITE_SHMEM_ADDR (0x9fc200000ULL)
#define RPMSG_LITE_SHMEM_ADDR (0x900000000ULL)
#define RPMSG_LITE_SHMEM_SIZE (16UL * 1024UL * 1024UL)

/* RPMsg-Lite platform link id validation. */
#define RL_PLATFORM_HIGHEST_LINK_ID (0U)

/* Virtqueue IDs are derived from link_id and vq index (0/1). */
#define RL_GET_VQ_ID(link_id, vq_id) (((link_id) << 1U) + (vq_id))

/*
 * Shared memory configuration provided by platform layer.
 *
 * Note: If RL_ALLOW_CUSTOM_SHMEM_CONFIG is enabled (see rpmsg_default_config.h),
 *       this struct must be filled in and returned by platform_get_custom_shmem_config().
 */
typedef struct rpmsg_platform_shmem_config
{
    uint32_t buffer_payload_size;
    uint32_t buffer_count;
    uint32_t vring_size;
    uint32_t vring_align;
} rpmsg_platform_shmem_config_t;

/* RPMsg-Lite role used in this platform. */
typedef enum rpmsg_platform_role
{
    RPMSG_PLATFORM_ROLE_REMOTE = 0,
    RPMSG_PLATFORM_ROLE_MASTER = 1,
} rpmsg_platform_role_t;

/*
 * Platform API required by RPMsg-Lite (rpmsg_lite.c).
 */
int32_t platform_get_custom_shmem_config(uint32_t link_id, rpmsg_platform_shmem_config_t *shmem_config);
int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data);
int32_t platform_deinit_interrupt(uint32_t vector_id);

#if defined(RL_USE_ENVIRONMENT_CONTEXT) && (RL_USE_ENVIRONMENT_CONTEXT == 1)
int32_t platform_notify(void *platform_context, uint32_t vector_id);
#else
int32_t platform_notify(uint32_t vector_id);
#endif

typedef void (*env_isr_t)(uint32_t vector);

/*
 * Optional helpers used by the BSP to configure the platform layer
 * (e.g. MSI-X / timer mapping).
 */
void platform_set_role(rpmsg_platform_role_t role);
void platform_set_timer_index(uint32_t timer_index);

/*
 * Additional platform API required by environment layer
 */
int32_t platform_in_isr(void);
int32_t platform_init(void);
int32_t platform_deinit(void);
uintptr_t platform_vatopa(void *address);
void *platform_patova(uintptr_t address);
int32_t platform_interrupt_enable(uint32_t vector_id);
int32_t platform_interrupt_disable(uint32_t vector_id);
void platform_map_mem_region(uint32_t va, uint32_t pa, uint32_t size, uint32_t flags);
void platform_cache_all_flush_invalidate(void);
void platform_cache_disable(void);

#endif /* RPMSG_PLATFORM_H_ */