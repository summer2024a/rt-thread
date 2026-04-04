/*
 * Copyright (C) 2017 C-SKY Microsystems Co., Ltd. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/******************************************************************************
 * @file     drv_timer.h
 * @brief    header file for timer driver
 * @version  V1.0
 * @date     02. June 2017
 ******************************************************************************/
#ifndef __DRV_TIMER_H
#define __DRV_TIMER_H

#include <rtthread.h>
#include <rtdevice.h>
#include "lynxi.h"

/* following defines should be used for structure members */
#define __IM  volatile const /*! Defines 'read only' structure member permissions */
#define __OM  volatile       /*! Defines 'write only' structure member permissions */
#define __IOM volatile       /*! Defines 'read / write' structure member permissions */

/*
 *  define the bits for TxControl
 */
#define DW_TIMER_TXCONTROL_ENABLE      (1UL << 0)
#define DW_TIMER_TXCONTROL_MODE        (1UL << 1)
#define DW_TIMER_TXCONTROL_INTMASK     (1UL << 2)

#define DW_TIMER_INIT_DEFAULT_VALUE     0x7ffffff

typedef struct {
    __IOM uint32_t TxLoadCount;              /* Offset: 0x000 (R/W)  Receive buffer register */
    __IM uint32_t TxCurrentValue;            /* Offset: 0x004 (R)  Transmission hold register */
    __IOM uint8_t TxControl: 4;              /* Offset: 0x008 (R/W)  Clock frequency division low section register */
    uint8_t  RESERVED0[3];
    __IM uint8_t TxEOI: 1;                   /* Offset: 0x00c (R)  Clock frequency division high section register */
    uint8_t  RESERVED1[3];
    __IM uint8_t TxIntStatus: 1;             /* Offset: 0x010 (R)  Interrupt enable register */
    uint8_t  RESERVED2[3];
} dw_timer_reg_t;


/// definition for timer handle.
typedef void *timer_handle_t;

/*----- TIMER Control Codes: Mode -----*/
typedef enum {
    TIMER_MODE_FREE_RUNNING                 = 0,   ///< free running mode
    TIMER_MODE_RELOAD                              ///< reload mode
} timer_mode_e;

/**
\brief TIMER Status
*/
typedef struct {
    uint32_t active   : 1;                        ///< timer active flag
    uint32_t timeout  : 1;                        ///< timeout flag
} timer_status_t;

/**
\brief TIMER Event
*/
typedef enum {
    TIMER_EVENT_TIMEOUT  = 0   ///< time out event
} timer_event_e;

typedef void (*timer_event_cb_t)(timer_event_e event, void *arg);   ///< Pointer to \ref timer_event_cb_t : TIMER Event call back.

/**
\brief TIMER Device Driver Capabilities.
*/
typedef struct {
    uint32_t interrupt_mode          : 1;      ///< supports Interrupt mode
} timer_capabilities_t;

timer_handle_t dw_timer_initialize(int32_t idx, timer_event_cb_t cb_event, void *arg);

int dw_timer_uninitialize(timer_handle_t handle);

void dw_timer_irqhandler(int idx);

int32_t dw_timer_start(timer_handle_t handle, uint32_t apbfreq);

int32_t dw_timer_set_timeout(timer_handle_t handle, uint32_t timeout);

int32_t dw_timer_config(timer_handle_t handle, timer_mode_e mode);

int32_t rpmsg_timer_prepare(timer_handle_t handle);

void rpmsg_timer_irqhandler(timer_handle_t priv);

#endif /* __DRV_TIMER_H */

