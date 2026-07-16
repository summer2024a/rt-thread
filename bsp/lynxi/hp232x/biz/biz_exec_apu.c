/*
 * biz_exec_apu.c — HP640 APU-related exec tasks (DgbRead, APUDebug, PWM, ApuClock).
 */
#include "biz_modules.h"

#ifdef BIZ_MOD_EXEC_APU

#include <rtthread.h>
#include <rthw.h>
#include "biz_exec_handlers.h"
#include "biz_emmc.h"
#include "drv_apu.h"
#include "drv_emmc.h"
#include "biz_log.h"
#include "hp232x_mmu.h"
#include "tick.h"

#define REG_APU_CTRL_ADDR           0x12500074UL
#define REG_APU_CTRL_CLK            0x02U
#define REG_APU_CTRL_RST            0x01U
#define APU_MDBG_BASE               0x00188500UL

static inline void mmio_write32(uint64_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static inline uint32_t mmio_read32(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

int biz_exec_apu_debug_read(const HP640_Task *task)
{
    int index, i;
    const int wait_times = 0x50001;
    uint64_t mdbg = HP232X_APU_BASE + APU_MDBG_BASE;

    if (!task)
        return BIZ_ERR_CLI_PARAM;

    mmio_write32(mdbg + 0x50, ((uint32_t)task->core_id << 16) | 0x0001U);

    for (index = 0; index < (int)task->size; index += 4)
    {
        mmio_write32(mdbg + 0x54, (uint32_t)(task->src_apu + index));
        mmio_write32(mdbg + 0x4c, 0x01U);

        for (i = 0; i < wait_times; i++)
        {
            rt_hw_us_delay(10);
            if ((mmio_read32(mdbg + 0x68) & 0x1FU) == 0x10U)
                break;
        }

        if (i == wait_times)
        {
            BIZ_ERROR("DgbRead timeout idx=%d core=%d\n", index, task->core_id);
            return BIZ_ERR_PRIM_TIMEOUT;
        }

        *(uint32_t *)(uintptr_t)(task->dst_addr + index) = mmio_read32(mdbg + 0x64);
    }

    return BIZ_SUCCESS;
}

int biz_exec_apu_pwm(const HP640_Task *task)
{
    rt_uint32_t d_on, d_off;
    rt_uint32_t i, t0;
    rt_uint64_t ts, te;

    if (!task)
        return BIZ_ERR_CLI_PARAM;

    d_on = task->duration * task->count * 1000U;
    d_off = task->duration * (task->total - task->count) * 1000U;

    t0 = rt_tick_get();
    for (i = 0; i < task->times; i++)
    {
        mmio_write32(REG_APU_CTRL_ADDR, 1U);
        ts = rt_tick_get();
        te = rt_tick_get();
        while (((te - ts) < d_off) || ((ts - te) > 0xFFFFU))
            te = rt_tick_get();

        if (d_on == 0)
            continue;

        mmio_write32(REG_APU_CTRL_ADDR, 3U);
        ts = rt_tick_get();
        te = rt_tick_get();
        while (((te - ts) < d_on) || ((ts - te) > 0xFFFFU))
            te = rt_tick_get();
    }

    BIZ_DEBUG("PWM done, cost=%u ticks\n", rt_tick_get() - t0);
    return BIZ_SUCCESS;
}

int biz_exec_apu_clock(const HP640_Task *task)
{
    if (!task)
        return BIZ_ERR_CLI_PARAM;

    if (drv_apu_pll_set(task->apu_config_val) != 0)
        return BIZ_ERR_NORMAL;

    return biz_emmc_report_heartbeat();
}

int biz_apu_dump_failed_status(uint32_t chip_id, unsigned char *addr, unsigned int size);

int biz_exec_apu_debug(const HP640_Task *task)
{
    if (!task)
        return BIZ_ERR_CLI_PARAM;

    switch (task->debug_type)
    {
    case 0:
        return biz_apu_dump_failed_status(task->chip_id,
            (unsigned char *)(uintptr_t)task->dst_addr, task->size);
    default:
        return BIZ_ERR_PRIM_PARAM;
    }
}

#endif /* BIZ_MOD_EXEC_APU */
