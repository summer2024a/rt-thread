/*
 * biz_ipc.h — Cross-core MCU error notification (CPU1 -> CPU0).
 */

#ifndef BIZ_IPC_H__
#define BIZ_IPC_H__

#include <stdint.h>
#include "biz_error_code.h"

struct biz_mcu_err_notify {
    volatile uint32_t pending;
    uint16_t code;
    biz_err_code_t err_code;
};

void biz_ipc_init(void);
void biz_mcu_err_post(biz_err_code_t err_code);
void biz_mcu_err_drain(void);
void biz_mcu_err_bh_entry(void *param);

#endif /* BIZ_IPC_H__ */
