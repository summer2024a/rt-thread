/*
 * drv_i2c.h — DesignWare I2C slave + MCU mailbox / READ_LOG for HP232x BSP.
 *
 * MCU Mode B (proxy): Mem_Write/Read reg=0x00 command mailbox — see biz_i2c_proxy.h.
 * Legacy: READ_LOG (0x01) and drv_i2c_mcu_prepare_error().
 */

#ifndef DRV_I2C_H__
#define DRV_I2C_H__

#include <stdint.h>
#include "biz_error_code.h"

#define DRV_I2C0_BASE               0x10002000UL
#define DRV_I2C_MCU_DEFAULT_ADDR    0x50U
#define DRV_I2C_SAR_CHIP_0          0x34U
#define DRV_I2C_ADDR_MIN            0x08U
#define DRV_I2C_ADDR_MAX            0x77U

int drv_i2c_mcu_resolve_addr(void);

int drv_i2c_is_ready(void);

typedef enum {
    DRV_I2C_SLAVE_READ_REQUESTED = 0,
    DRV_I2C_SLAVE_WRITE_REQUESTED,
    DRV_I2C_SLAVE_READ_PROCESSED,
    DRV_I2C_SLAVE_WRITE_RECEIVED,
    DRV_I2C_SLAVE_STOP,
} drv_i2c_slave_event_t;

int drv_i2c_init(void);
void drv_i2c_bh_entry(void *param);

uint8_t drv_i2c_mcu_get_addr(void);
int drv_i2c_mcu_prepare_error(uint16_t error_data);
int drv_i2c_mcu_prepare_log(void);
void drv_i2c_mcu_reset(void);

#endif /* DRV_I2C_H__ */
