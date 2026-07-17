/*
 * biz_ipc.c — CPU1 eMMC error post, CPU0 I2C/GPIO notify.
 */

#include <rtthread.h>
#include "biz_ipc.h"
#include "drv_i2c.h"
#include "drv_gpio_mcu.h"
#include "drv_flash.h"
#include "biz_log.h"

static struct biz_mcu_err_notify s_notify;
static rt_sem_t s_err_sem;
static uint32_t s_triggered_mask;
static uint32_t s_init_flag;

static uint16_t err_code_to_mcu_data(biz_err_code_t err)
{
    switch (err)
    {
    case BIZ_ERR_EMMC_INIT:
        return 0x01;
    case BIZ_ERR_EMMC_CMD_CRC:
        return 0x08;
    case BIZ_ERR_EMMC_DATA_CRC:
        return 0x10;
    case BIZ_ERR_EMMC_DLL_SCAN_FAILED:
        return 0x20;
    case BIZ_ERR_EMMC_DATA_SIZE:
        return 0x40;
    case BIZ_ERR_EMMC_TUNING_FAILED:
        return 0x80;
    case BIZ_ERR_EMMC_SET_BLK_LEN:
        return 0x100;
    default:
        return (uint16_t)err;
    }
}

void biz_ipc_init(void)
{
    s_err_sem = rt_sem_create("mcu_err", 0, RT_IPC_FLAG_FIFO);
}

void biz_mcu_err_post(biz_err_code_t err_code)
{
    uint16_t data;

    if (err_code == BIZ_ERR_EMMC_STATE_TIMEOUT ||
        err_code == BIZ_ERR_EMMC_CMD_TIMEOUT ||
        err_code == BIZ_ERR_EMMC_DATA_TIMEOUT)
    {
        return;
    }

    if (s_init_flag != 0x12345678U)
    {
        s_triggered_mask = 0;
        s_init_flag = 0x12345678U;
    }

    if (s_triggered_mask & (1U << err_code))
        return;

    s_triggered_mask |= (1U << err_code);
    data = err_code_to_mcu_data(err_code);

    if (err_code == BIZ_ERR_EMMC_DLL_SCAN_FAILED)
    {
        /* Word store — never write uint16_t with len=4 (garbage high bytes). */
        uint32_t word = data;

        drv_flash_write(&word, 4, DRV_FLASH_EMMC_ERROR_ADDR);
    }

    s_notify.err_code = err_code;
    s_notify.code = data;
    s_notify.pending = 1;

    BIZ_INFO("MCU err post: code=%d data=0x%x\n", err_code, data);

    if (s_err_sem)
        rt_sem_release(s_err_sem);
}

void biz_mcu_err_drain(void)
{
    if (!s_notify.pending)
        return;

    s_notify.pending = 0;
    drv_gpio_mcu_error_init();
    drv_i2c_mcu_prepare_error(s_notify.code);
    drv_gpio_mcu_error_pulse();
}

void biz_mcu_err_bh_entry(void *param)
{
    (void)param;

    while (1)
    {
        if (s_err_sem)
            rt_sem_take(s_err_sem, RT_WAITING_FOREVER);
        biz_mcu_err_drain();
    }
}
