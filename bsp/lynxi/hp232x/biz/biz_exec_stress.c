/*
 * biz_exec_stress.c — eMMC stress exec task (hp640 process_stress simplified).
 *
 * Data buffers use IRAM1 low 256KB scratch @ 0x100000000 (hp640 DDR_IRAM_ADDR),
 * not linked .bss — MMU maps WB; flush/invalidate around CPU and DMA use.
 */
#include "biz_modules.h"

#ifdef BIZ_MOD_EXEC_STRESS

#include <string.h>
#include <stdlib.h>
#include "board.h"
#include "biz_exec_handlers.h"
#include "drv_emmc.h"
#include "biz_ipc.h"
#include "biz_log.h"
#include <rthw.h>

/* hp640 spl_cmd.c MAX_BLK=64; recv @ DDR_IRAM_ADDR + 0x10000 */
#define STRESS_MAX_BLK          64
#define STRESS_BUF_SIZE         (EMMC_BLK_SIZE * STRESS_MAX_BLK)
#define BIZ_STRESS_WR_ADDR      IRAM1_START
#define BIZ_STRESS_RD_ADDR      (IRAM1_START + 0x10000UL)

static uint8_t *stress_wr_buf(void)
{
    return (uint8_t *)(uintptr_t)BIZ_STRESS_WR_ADDR;
}

static uint8_t *stress_rd_buf(void)
{
    return (uint8_t *)(uintptr_t)BIZ_STRESS_RD_ADDR;
}

static void stress_cache_flush(void *addr, rt_size_t len)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, addr, len);
    rt_hw_barrier(dsb, sy);
}

static void stress_cache_invalidate(void *addr, rt_size_t len)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, addr, len);
    rt_hw_barrier(dsb, sy);
}

static int process_stress(unsigned int blk_cnt, unsigned int type,
                          unsigned long src_emmc, unsigned long times,
                          unsigned int *tcost_ms)
{
    int read_mode = 0, write_mode = 0;
    unsigned long step = 100000UL;
    unsigned long s, cost;
    rt_tick_t t0;
    int ret = BIZ_SUCCESS;
    uint16_t blk_size = EMMC_BLK_SIZE;
    rt_size_t xfer_bytes = (rt_size_t)blk_size * blk_cnt;
    uint8_t *wr = stress_wr_buf();
    uint8_t *rd = stress_rd_buf();

    if (type == 1)
        read_mode = 1;
    else if (type == 2)
        write_mode = 1;
    else if (type == 3 || type == 4)
    {
        read_mode = 1;
        write_mode = 1;
    }
    else
        return BIZ_ERR_PRIM_PARAM;

    if (xfer_bytes > STRESS_BUF_SIZE)
        return BIZ_ERR_CLI_PARAM;

    if (times != 0 && times < step)
        step = times;

    if (write_mode)
    {
        for (unsigned int i = 0; i < xfer_bytes; i++)
            wr[i] = (uint8_t)(rand() % 255);
        stress_cache_flush(wr, xfer_bytes);
    }

    t0 = rt_tick_get();
    if (times == 0)
    {
        while (1)
        {
            if (write_mode)
            {
                ret = drv_emmc_write_blocks((uint32_t)src_emmc, wr,
                                            (uint16_t)blk_cnt, blk_size);
                if (ret != BIZ_SUCCESS)
                    break;
            }
            if (read_mode)
            {
                stress_cache_invalidate(rd, xfer_bytes);
                ret = drv_emmc_read_blocks((uint32_t)src_emmc, rd,
                                           (uint16_t)blk_cnt, blk_size);
                if (ret != BIZ_SUCCESS)
                    break;
            }
        }
    }
    else
    {
        for (s = 0; s < times; s++)
        {
            if (type == 4)
            {
                stress_cache_invalidate(wr, xfer_bytes);
                for (unsigned int i = 0; i < xfer_bytes; i++)
                    wr[i] = (uint8_t)(rand() % 255);
                stress_cache_flush(wr, xfer_bytes);
            }
            if (write_mode)
            {
                ret = drv_emmc_write_blocks((uint32_t)src_emmc, wr,
                                            (uint16_t)blk_cnt, blk_size);
                if (ret != BIZ_SUCCESS)
                    break;
            }
            if (read_mode)
            {
                stress_cache_invalidate(rd, xfer_bytes);
                ret = drv_emmc_read_blocks((uint32_t)src_emmc, rd,
                                           (uint16_t)blk_cnt, blk_size);
                if (ret != BIZ_SUCCESS)
                    break;
            }
        }
    }

    cost = rt_tick_get() - t0;
    if (tcost_ms)
        *tcost_ms = (unsigned int)cost;

    return ret;
}

int biz_exec_stress(const HP640_Task *task)
{
    unsigned int tcost = 0;
    unsigned int cnt = 0;
    unsigned char combined[8];
    uint32_t emmc_addr;
    int ret;

    if (!task)
        return BIZ_ERR_CLI_PARAM;

    emmc_addr = HP640_STRESS_TIME_REG_ADDR;
    emmc_addr |= (0x80U | (8U / 4U - 1U)) << 24;

    if (task->times == 0)
    {
        while (1)
        {
            cnt = (cnt + 1) & 0xFFFFU;
            ret = process_stress(task->blk_cnt, task->type, task->src_addr,
                                 100000UL, &tcost);
            if (ret != BIZ_SUCCESS)
                return ret;

            memcpy(combined, &tcost, 4);
            memcpy(combined + 4, &cnt, 4);
            stress_cache_flush(combined, sizeof(combined));
            ret = drv_emmc_write_blocks(emmc_addr, combined, 1, BIZ_BLK_SIZE);
            if (ret != BIZ_SUCCESS)
            {
                biz_mcu_err_post((biz_err_code_t)ret);
                return ret;
            }
        }
    }

    for (unsigned long i = 0; i < task->times; i++)
    {
        cnt = (cnt + 1) & 0xFFFFU;
        ret = process_stress(task->blk_cnt, task->type, task->src_addr,
                             task->times, &tcost);
        if (ret != BIZ_SUCCESS)
            return ret;

        memcpy(combined, &tcost, 4);
        memcpy(combined + 4, &cnt, 4);
        stress_cache_flush(combined, sizeof(combined));
        ret = drv_emmc_write_blocks(emmc_addr, combined, 1, BIZ_BLK_SIZE);
        if (ret != BIZ_SUCCESS)
        {
            biz_mcu_err_post((biz_err_code_t)ret);
            return ret;
        }
    }

    return BIZ_SUCCESS;
}

#endif /* BIZ_MOD_EXEC_STRESS */
