/*
 * biz_config.c — Default hp640_config values.
 */

#include "biz_config.h"
#include "biz_log.h"
#include <string.h>

HP640_Config biz_config;

void biz_config_init(void)
{
    unsigned char init_level;

    memset(&biz_config, 0, sizeof(biz_config));
    biz_config.chip_id = 0;
    /* 0: no periodic HB; emmc_biz still reports once at power-on. */
    biz_config.heart_beat_interval = 0;
    biz_config.unlimited_apu_task = 0;
    biz_config.self_test_report_base_address = 0x400000U;
    biz_config.heart_beat_report_base_address = 0x400000U;
    biz_config.task_result_report_base_address = 0x400208U;

    /* hp640: hp640_config.log_level = get_loglevel(); set_loglevel_pt(&...).
     * Host CMD_Config writes biz_config in place — filter must point here. */
    init_level = biz_log_get_level();
    biz_config.log_level = init_level;
    biz_log_set_level_pt(&biz_config.log_level);
}
