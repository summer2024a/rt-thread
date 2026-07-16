/*
 * biz_emmc.h — eMMC business APIs (heartbeat, task query/exec, main loop).
 */
#ifndef BIZ_EMMC_H__
#define BIZ_EMMC_H__

#include <rtthread.h>
#include "biz_host_proto.h"

const char *biz_hp640_cmd_name(unsigned char cmd);

int biz_emmc_query_task(HP640_Task *task_list, int max_tasks, uint32_t *out_rounds);
int biz_emmc_report_heartbeat(void);
int biz_emmc_process_heartbeat(void);
int biz_emmc_exec_tasks(HP640_Task *task_list, int task_count, int *task_ret);
int biz_emmc_report_task_result(int *task_ret, unsigned int len);

void biz_emmc_biz_entry(void *param);

int biz_emmc_get_apu_init_flag(void);
void biz_emmc_set_apu_init_flag(int flag);

#ifdef BSP_BIZ_PHASE_STATS
void biz_phase_note_query_us(rt_uint64_t us);
void biz_phase_note_query_rounds(rt_uint32_t rounds);
void biz_phase_note_task_head(unsigned char cmd, unsigned int tag, rt_uint64_t bd_or_src);
void biz_phase_note_report_us(rt_uint64_t us);
void biz_phase_note_cmd(unsigned char cmd, rt_uint64_t us);
rt_uint64_t biz_phase_now_us(void);
void biz_phase_pkg_done(rt_uint64_t query_us, rt_uint32_t qt_rounds, rt_uint64_t round_us);
void biz_phase_dump(void);
void biz_phase_reset(void);
#endif

void biz_emmc_dll_boot_init(void);
uint8_t biz_emmc_dll_offset_get(void);

#endif /* BIZ_EMMC_H__ */
