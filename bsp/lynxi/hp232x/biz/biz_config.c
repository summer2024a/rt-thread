/*
 * biz_config.c — Default hp640_config values.
 */

#include "biz_config.h"
#include <string.h>

HP640_Config biz_config;

void biz_config_init(void)
{
    memset(&biz_config, 0, sizeof(biz_config));
    biz_config.chip_id = 0;
    /* 0: no periodic HB; emmc_biz still reports once at power-on. */
    biz_config.heart_beat_interval = 0;
    biz_config.unlimited_apu_task = 0;
    biz_config.self_test_report_base_address = 0x400000U;
    biz_config.heart_beat_report_base_address = 0x400000U;
    biz_config.task_result_report_base_address = 0x400208U;
}
