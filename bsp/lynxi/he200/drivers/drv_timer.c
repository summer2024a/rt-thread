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
 * @file     drv_timer.c
 * @brief    CSI Source File for timer Driver
 * @version  V1.0
 * @date     02. June 2017
 ******************************************************************************/
#include <string.h>
#include <stdlib.h>
#include "drv_timer.h"
#include "drv_errno.h"
#include "lynxi.h"
#include <drivers/clock_time.h>
#include <rthw.h>
#define DBG_TAG "drv_timer"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

#define ERR_TIMER(errno) (DRV_ERRNO_TIMER_BASE | errno)

#define TIMER_NULL_PARAM_CHK(para)                  \
    do {                                        \
        if (para == NULL) {                     \
            return ERR_TIMER(DRV_ERROR_PARAMETER);   \
        }                                       \
    } while (0)

typedef struct {
    size_t base;
    uint32_t irq;
    timer_event_cb_t cb_event;
    uint32_t timeout;                  ///< the set time (us)
    uint32_t timeout_flag;
    void *arg;
} dw_timer_priv_t;

static dw_timer_priv_t timer_instance[CONFIG_TIMER_NUM];

static const timer_capabilities_t timer_capabilities = {
    .interrupt_mode = 1  ///< supports Interrupt mode
};

/**
  \brief      Make all the timers in the idle state.
  \param[in]  pointer to timer register base
*/
static void timer_deactive_control(dw_timer_reg_t *addr)
{
    /* stop the corresponding timer */
    addr->TxControl &= ~DW_TIMER_TXCONTROL_ENABLE;
    /* Disable interrupt. */
    addr->TxControl |= DW_TIMER_TXCONTROL_INTMASK;
}

void dw_timer_irqhandler(int idx)
{
    dw_timer_priv_t *timer_priv = &timer_instance[idx];
    timer_priv->timeout_flag = 1;

    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    addr->TxEOI;

    if (timer_priv->cb_event) {
        return timer_priv->cb_event(TIMER_EVENT_TIMEOUT, timer_priv->arg);
    }

}

int32_t __attribute__((weak)) target_get_timer_count(void)
{
    return 0;
}

int32_t target_get_timer(uint32_t idx, size_t *base, uint32_t *irq)
{

    *base = stimer_base_addr + 20 * idx;
    *irq = TIMER_IRQ_START + idx;

    return idx;
}

/**
  \brief       get timer instance count.
  \return      timer instance count
*/
int32_t dw_timer_get_instance_count(void)
{
    return target_get_timer_count();
}

/**
  \brief       Initialize TIMER Interface. 1. Initializes the resources needed for the TIMER interface 2.registers event callback function
  \param[in]   idx  instance timer index
  \param[in]   cb_event  Pointer to \ref timer_event_cb_t
  \return      pointer to timer instance
*/
timer_handle_t dw_timer_initialize(int32_t idx, timer_event_cb_t cb_event, void *arg)
{
    if (idx < 0 || idx >= CONFIG_TIMER_NUM) {
        return NULL;
    }

    size_t base = 0u;
    uint32_t irq = 0u;

    int32_t real_idx = target_get_timer(idx, &base, &irq);

    if (real_idx != idx) {
        return NULL;
    }

    dw_timer_priv_t *timer_priv = &timer_instance[idx];
    timer_priv->base = base;
    timer_priv->irq  = irq;
    timer_priv->arg  = arg;

    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);
    timer_priv->timeout = DW_TIMER_INIT_DEFAULT_VALUE;

    timer_deactive_control(addr);
    timer_priv->cb_event = cb_event;

    return (timer_handle_t)timer_priv;
}

/**
  \brief       De-initialize TIMER Interface. stops operation and releases the software resources used by the interface
  \param[in]   handle timer handle to operate.
  \return      error code
*/
int32_t dw_timer_uninitialize(timer_handle_t handle)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = (dw_timer_priv_t *)handle;
    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    timer_deactive_control(addr);
    timer_priv->cb_event = NULL;

    // drv_nvic_disable_irq(timer_priv->irq);
    return 0;
}

/**
  \brief       Get driver capabilities.
  \param[in]   handle timer handle to operate.
  \return      \ref timer_capabilities_t
*/
timer_capabilities_t dw_timer_get_capabilities(timer_handle_t handle)
{
    return timer_capabilities;
}

/**
  \brief       config timer mode.
  \param[in]   handle timer handle to operate.
  \param[in]   mode      \ref timer_mode_e
  \return      error code
*/
int32_t dw_timer_config(timer_handle_t handle, timer_mode_e mode)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = handle;
    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    switch (mode) {
        case TIMER_MODE_FREE_RUNNING:
            addr->TxControl &= ~DW_TIMER_TXCONTROL_MODE;
            break;

        case TIMER_MODE_RELOAD:
            addr->TxControl |= DW_TIMER_TXCONTROL_MODE;
            break;

        default:
            return ERR_TIMER(DRV_ERROR_PARAMETER);
    }

    return 0;
}

/**
  \brief       Set timer.
  \param[in]   instance  timer instance to operate.
  \param[in]   timeout the timeout value in microseconds(us).
  \return      error code
*/
int32_t dw_timer_set_timeout(timer_handle_t handle, uint32_t timeout)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = handle;
    timer_priv->timeout = timeout;
    return 0;
}

/**
  \brief       Start timer.
  \param[in]   handle timer handle to operate.
  \return      error code
*/
int32_t dw_timer_start(timer_handle_t handle, uint32_t apbfreq)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = handle;

    timer_priv->timeout_flag = 0;

    uint32_t min_us = apbfreq / 1000000;

    if ((timer_priv->timeout < min_us) || (timer_priv->timeout > 0xffffffff / min_us)) {
        return ERR_TIMER(DRV_ERROR_PARAMETER);
    }

    uint32_t load = (uint32_t)(timer_priv->timeout * min_us);

    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    addr->TxLoadCount = load;                           /* load time(us) */
    addr->TxControl &= ~DW_TIMER_TXCONTROL_ENABLE;      /* disable the timer */
    addr->TxControl |= DW_TIMER_TXCONTROL_ENABLE;       /* enable the corresponding timer */
    addr->TxControl &= ~DW_TIMER_TXCONTROL_INTMASK;     /* enable interrupt */

    return 0;
}

/**
  \brief       prepare timer. no start timer.
  \param[in]   handle timer handle to operate.
  \return      error code
*/
int32_t rpmsg_timer_prepare(timer_handle_t handle)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = handle;
    uint32_t load = 1;

    timer_priv->timeout_flag = 0;

    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    addr->TxControl &= ~DW_TIMER_TXCONTROL_ENABLE;      /* disable the timer */
    addr->TxControl &= ~DW_TIMER_TXCONTROL_MODE;        /* free running mode */
    addr->TxControl &= ~DW_TIMER_TXCONTROL_INTMASK;     /* enable interrupt */
    addr->TxLoadCount = load;                           /* load time(us) */

    return 0;
}

void rpmsg_timer_irqhandler(timer_handle_t priv)
{
    if (!priv) {
        return;
    }

    dw_timer_priv_t *timer_priv = priv;
    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    addr->TxControl &= ~DW_TIMER_TXCONTROL_ENABLE;      /* disable the timer */
    addr->TxEOI;

    if (timer_priv->cb_event) {
        return timer_priv->cb_event(TIMER_EVENT_TIMEOUT, timer_priv->arg);
    }
}

/**
  \brief       Stop timer.
  \param[in]   handle timer handle to operate.
  \return      error code
*/
int32_t dw_timer_stop(timer_handle_t handle)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = handle;
    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    addr->TxControl |= DW_TIMER_TXCONTROL_INTMASK;      /* enable interrupt */
    addr->TxControl &= ~DW_TIMER_TXCONTROL_ENABLE;      /* disable the timer */

    return 0;
}

/**
  \brief       suspend timer.
  \param[in]   instance  timer instance to operate.
  \return      error code
*/
int32_t dw_timer_suspend(timer_handle_t handle)
{
    TIMER_NULL_PARAM_CHK(handle);

    return ERR_TIMER(DRV_ERROR_UNSUPPORTED);
}

/**
  \brief       resume timer.
  \param[in]   handle timer handle to operate.
  \return      error code
*/
int32_t dw_timer_resume(timer_handle_t handle)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = handle;
    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    addr->TxControl &= ~DW_TIMER_TXCONTROL_ENABLE;      /* stop the corresponding timer */
    addr->TxControl &= DW_TIMER_TXCONTROL_ENABLE;       /* restart the corresponding timer */

    return 0;
}

/**
  \brief       get timer current value
  \param[in]   handle timer handle to operate.
  \param[in]   value     timer current value
  \return      error code
*/
int32_t dw_timer_get_current_value(timer_handle_t handle, uint32_t *value)
{
    TIMER_NULL_PARAM_CHK(handle);

    dw_timer_priv_t *timer_priv = handle;
    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    *value = addr->TxCurrentValue;
    return 0;
}

/**
  \brief       Get TIMER status.
  \param[in]   handle timer handle to operate.
  \return      TIMER status \ref timer_status_t
*/
timer_status_t dw_timer_get_status(timer_handle_t handle)
{
    timer_status_t timer_status = {0};

    if (handle == NULL) {
        return timer_status;
    }

    dw_timer_priv_t *timer_priv = handle;
    dw_timer_reg_t *addr = (dw_timer_reg_t *)(timer_priv->base);

    if (addr->TxControl & DW_TIMER_TXCONTROL_ENABLE) {
        timer_status.active = 1;
    }

    if (timer_priv->timeout_flag == 1) {
        timer_status.timeout = 1;
    }

    return timer_status;
}

struct clock_timer_device
{
    struct rt_clock_timer_device parent;
    dw_timer_priv_t *timer_priv;
    rt_uint32_t  timer_id;
    char name[32];
    rt_uint32_t apbfreq;
    rt_uint32_t cnt;
};

static void drv_timer_event_cb(timer_event_e event, void *arg)
{
    if (TIMER_EVENT_TIMEOUT == event)
        LOG_I("timer timeout");
}

static void drv_timer_init(struct rt_clock_timer_device *timer, rt_uint32_t state)
{
    struct clock_timer_device *systimer =
            rt_container_of(timer, struct clock_timer_device, parent);

    LOG_I("timer init");
    if (state) {
        timer_handle_t *hdl = dw_timer_initialize(systimer->timer_id, drv_timer_event_cb, NULL);
        if (!hdl) {
            LOG_E("timer start failed");
            return;
        }
    } else {
        dw_timer_uninitialize(systimer->timer_priv);
    }

    return;
}

static rt_err_t drv_timer_start(struct rt_clock_timer_device *timer, rt_uint32_t cnt, rt_clock_timer_mode_t mode)
{
    rt_err_t ret = 0;
    struct clock_timer_device *systimer =
            rt_container_of(timer, struct clock_timer_device, parent);

    LOG_I("timer start");
    if (mode == CLOCK_TIMER_MODE_PERIOD)
        dw_timer_config(systimer->timer_priv, TIMER_MODE_RELOAD);
    else
        dw_timer_config(systimer->timer_priv, TIMER_MODE_FREE_RUNNING);

    ret = dw_timer_start(systimer->timer_priv, cnt);
    if (ret != 0) {
        LOG_E("timer start failed");
        return ret;
    }

    return 0;
}
static void drv_timer_stop(struct rt_clock_timer_device *timer)
{
    struct clock_timer_device *systimer =
            rt_container_of(timer, struct clock_timer_device, parent);

    dw_timer_stop(systimer->timer_priv);

    return;
}
static rt_uint32_t drv_timer_count_get(struct rt_clock_timer_device *timer)
{
    uint32_t value;
    struct clock_timer_device *systimer =
            rt_container_of(timer, struct clock_timer_device, parent);

    if (!dw_timer_get_current_value(systimer->timer_priv, &value))
        return value;
    else
        return 0;
}

static rt_err_t drv_timer_control(struct rt_clock_timer_device *timer, rt_uint32_t cmd, void *args)
{
    struct clock_timer_device *systimer;
    rt_err_t result = RT_EOK;

    RT_ASSERT(timer != RT_NULL);

    systimer = rt_container_of(timer, struct clock_timer_device, parent);

    switch (cmd)
    {
    case CLOCK_TIMER_CTRL_FREQ_SET:
    {
        if (args != RT_NULL)
        {
            systimer->apbfreq = *((rt_uint32_t *)args);
        }
        else
        {
            result = -RT_EINVAL;
        }
    }
    break;

    default:
        result = -RT_ENOSYS;
        break;
    }

    return result;
};

void dw_timer_isr(int vector, void *param)
{
    struct clock_timer_device *systimer = (struct clock_timer_device *)param;

    dw_timer_irqhandler(systimer->timer_id);
}

static struct rt_clock_timer_ops drv_timer_ops =
{
    .init = drv_timer_init,
    .start = drv_timer_start,
    .stop = drv_timer_stop,
    .count_get = drv_timer_count_get,
    .control = drv_timer_control,
};

struct rt_clock_timer_info drv_timer_info =
    {
        .maxfreq = 50000000,
        .minfreq = 2000,
        .maxcnt = 0xffffffff,
        .cntmode = CLOCK_TIMER_CNTMODE_UP,

};

static struct clock_timer_device _systimer =
{
    .timer_id = 1,
    .cnt = 0,
    .parent =
    {
        .ops = &drv_timer_ops,
        .info = &drv_timer_info,
    },
};

int rt_hw_systimer_init(void)
{

// #ifdef BSP_USING_SYSTIMER

    _systimer.timer_id = 0;
    _systimer.timer_priv = &timer_instance[_systimer.timer_id];
    int vector = TIMER_IRQ_START + _systimer.timer_id;
    rt_snprintf(_systimer.name, sizeof(_systimer.name), "systimer%d", _systimer.timer_id);
    rt_clock_timer_register(&_systimer.parent, _systimer.name, &_systimer);
    rt_hw_interrupt_install(vector, dw_timer_isr,  &_systimer, _systimer.name);
    rt_hw_interrupt_umask(vector);

// #endif

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_systimer_init);

int test_timer(int argc, char** argv)
{
    if (argc != 3)
        return -1;

    _systimer.timer_id = atoi(argv[1]);
    if (_systimer.timer_id > CONFIG_TIMER_NUM - 1)
        return -1;

    _systimer.timer_priv = &timer_instance[_systimer.timer_id];
    if (rt_strncmp(argv[2], "off", 3) == 0) {
        drv_timer_stop(&_systimer.parent);
    } else {
        int vector = TIMER_IRQ_START + _systimer.timer_id;
        drv_timer_init(&_systimer.parent, 1);
        drv_timer_start(&_systimer.parent, 1000000, CLOCK_TIMER_MODE_PERIOD);
        memset(_systimer.name, 0, sizeof(_systimer.name));
        rt_snprintf(_systimer.name, sizeof(_systimer.name), "systimer%d", _systimer.timer_id);
        rt_hw_interrupt_install(vector, dw_timer_isr,  &_systimer, _systimer.name);
        rt_hw_interrupt_umask(vector);
    }

    return 0;
}

MSH_CMD_EXPORT_ALIAS(test_timer, test_timer, test_timer <timerid> <on|off>);

