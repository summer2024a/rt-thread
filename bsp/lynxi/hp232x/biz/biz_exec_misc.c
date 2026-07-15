/*
 * biz_exec_misc.c — CRC32, Timer, SetTimestamp exec tasks.
 */
#include "biz_modules.h"

#ifdef BIZ_MOD_EXEC_MISC

#include <rtthread.h>
#include <rthw.h>
#include "biz_exec_handlers.h"
#include "biz_log.h"
#include "tick.h"

static rt_uint64_t s_timer_us_base[HP640_TAKS_PKG_LENGTH];

int biz_exec_crc32(const HP640_Task *task)
{
    uint32_t *src;
    uint32_t *ret;

    if (!task)
        return BIZ_ERR_CLI_PARAM;

    src = (uint32_t *)(uintptr_t)task->src_addr;
    ret = (uint32_t *)(uintptr_t)task->ret_addr;

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, src, task->size);

    if (task->chip_id == 0xffU)
        *ret = biz_crc32_calc(0, src, task->size);
    else
        *ret = biz_crc32_calc(*ret, src, task->size);

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, ret, sizeof(uint32_t));
    return BIZ_SUCCESS;
}

int biz_exec_timer(const HP640_Task *task, HP640_Task *task_base, int task_count)
{
    int idx;
    rt_uint64_t now_us;

    if (!task || !task_base || task_count <= 0)
        return BIZ_ERR_CLI_PARAM;

    now_us = (rt_uint64_t)rt_tick_get_millisecond() * 1000ULL;

    switch (task->type)
    {
    case 0:
        *(unsigned long *)(uintptr_t)task->dst_addr = 0;
        break;
    case 1:
        for (idx = 1; idx < task_count; idx++)
        {
            if (!(task_base[idx].flag & HP640_CMD_NOT_END))
                break;
            if (task_base[idx].cmd == HP640_CMD_Timer && task_base[idx].id == task->id)
            {
                if (task->id < HP640_TAKS_PKG_LENGTH)
                    s_timer_us_base[task->id] = now_us;
                task_base[idx].timer_begin = now_us;
                return BIZ_SUCCESS;
            }
        }
        return BIZ_ERR_PRIM_PARAM;
    case 2:
        *(unsigned long *)(uintptr_t)task->dst_addr +=
            now_us - (rt_uint64_t)task->timer_begin;
        break;
    default:
        return BIZ_ERR_PRIM_PARAM;
    }

    return BIZ_SUCCESS;
}

int biz_exec_set_timestamp(const HP640_Task *task)
{
    (void)task;
    BIZ_DEBUG("SetTimestamp (log timestamp hook TBD)\n");
    return BIZ_SUCCESS;
}

#endif /* BIZ_MOD_EXEC_MISC */
