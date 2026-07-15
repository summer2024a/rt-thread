/*
 * biz_exec_handlers.h — HP640 task command handlers (optional modules).
 */
#ifndef BIZ_EXEC_HANDLERS_H__
#define BIZ_EXEC_HANDLERS_H__

#include "biz_host_proto.h"
#include "biz_error_code.h"
#include <rtthread.h>

int biz_exec_apu_debug_read(const HP640_Task *task);
int biz_exec_apu_debug(const HP640_Task *task);
int biz_exec_apu_pwm(const HP640_Task *task);
int biz_exec_apu_clock(const HP640_Task *task);

int biz_exec_crc32(const HP640_Task *task);
int biz_exec_timer(const HP640_Task *task, HP640_Task *task_base, int task_count);
int biz_exec_set_timestamp(const HP640_Task *task);

int biz_exec_stress(const HP640_Task *task);
int biz_exec_pcie_stress(const HP640_Task *task);
int biz_exec_pcie_setup(const HP640_Task *task);

int biz_exec_self_test(void);
int biz_exec_emmc_dll_scan(void);

int biz_self_test_report(void);

void drv_pcie_boot_init(void);
int drv_pcie_is_rc(void);
int drv_pcie_set_mode(enum HP640_PCIE_MODE mode);
int drv_pcie_dma_test(uint32_t test_size, unsigned long times);

void biz_emmc_dll_boot_init(void);
uint8_t biz_emmc_dll_offset_get(void);

void biz_log_set_level(unsigned char level);

uint32_t biz_crc32_calc(uint32_t crc, const void *buf, rt_size_t len);

#endif /* BIZ_EXEC_HANDLERS_H__ */
