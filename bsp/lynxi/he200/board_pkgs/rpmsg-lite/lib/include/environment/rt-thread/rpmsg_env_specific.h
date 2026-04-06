/*
 * Copyright 2021 NXP
 * All rights reserved.
 *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**************************************************************************
 * FILE NAME
 *
 *       rpmsg_env_specific.h
 *
 * DESCRIPTION
 *
 *       This file contains baremetal specific constructions.
 *
 **************************************************************************/
#ifndef RPMSG_ENV_SPECIFIC_H_
#define RPMSG_ENV_SPECIFIC_H_

#include <rtthread.h>
#include <stdint.h>
#define DBG_TAG "rpmsg-lite"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>
#include "rpmsg_default_config.h"

typedef struct
{
    uint32_t src;
    void *data;
    uint32_t len;
} rpmsg_queue_rx_cb_data_t;

#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
typedef struct rt_mutex LOCK_STATIC_CONTEXT;
typedef struct rt_messagequeue rpmsg_static_queue_ctxt;
#endif

// 用宏定义实现 env_info, env_dbg, env_warn, env_error, env_debug, env_log, env_hex_dump
#define ENV_INFO(...) LOG_I(__VA_ARGS__)
#define ENV_DBG(...) LOG_D(__VA_ARGS__)
#define ENV_WARN(...) LOG_W(__VA_ARGS__)
#define ENV_ERROR(...) LOG_E(__VA_ARGS__)
#define ENV_DEBUG(...) LOG_D(__VA_ARGS__)
#define ENV_LOG(...) LOG_I(__VA_ARGS__)
#define ENV_HEX_DUMP(name, width, buf, size) LOG_HEX(name, width, buf, size)

#endif /* RPMSG_ENV_SPECIFIC_H_ */
