/*
 * biz_emmc_exec.c — HP640 task execution (exec_tasks port).
 */

#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include "board.h"
#include "drv_emmc.h"
#include "biz_config.h"
#include "biz_log.h"
#include "biz_ipc.h"
#include "drv_apu.h"
#include "drv_flash.h"
#include "biz_modules.h"
#include "biz_exec_handlers.h"
#include "biz_emmc.h"
#include "tick.h"

static inline void mmio_write8(uint64_t addr, uint8_t val)
{
    *(volatile uint8_t *)(uintptr_t)addr = val;
}

static inline void mmio_write16(uint64_t addr, uint16_t val)
{
    *(volatile uint16_t *)(uintptr_t)addr = val;
}

static inline void mmio_write32(uint64_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static inline void mmio_write64(uint64_t addr, uint64_t val)
{
    *(volatile uint64_t *)(uintptr_t)addr = val;
}

static inline uint32_t mmio_read32(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

#ifdef BSP_BIZ_PHASE_STATS
/*
 * Silent accumulate; dump via biz_phase_dump() / msh "phase".
 * No auto print every 10k or Round Stats spam.
 */
static rt_uint64_t s_ph_sum_query_us;
static rt_uint64_t s_ph_sum_report_us;
static rt_uint64_t s_ph_sum_round_us;
static rt_uint64_t s_ph_sum_qt_rounds;
static rt_uint64_t s_ph_execbd_us;
static rt_uint64_t s_ph_crc_us;
static rt_uint64_t s_ph_other_us;
static rt_uint32_t s_ph_execbd_n;
static rt_uint32_t s_ph_crc_n;
static rt_uint32_t s_ph_other_n;
static rt_uint32_t s_ph_total_n;
static rt_uint32_t s_ph_stale_total;
static rt_uint32_t s_ph_last_cmd;
static rt_uint32_t s_ph_last_tag;
static rt_uint64_t s_ph_last_bd;
static rt_uint32_t s_ph_have_last;
static rt_uint32_t s_ph_round_qt_rounds;
static rt_uint64_t s_ph_round_report_us;

static rt_uint64_t biz_ph_cntpct(void)
{
    rt_uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static rt_uint64_t biz_ph_cntfrq(void)
{
    rt_uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v ? v : 31250000ULL;
}

rt_uint64_t biz_phase_now_us(void)
{
    return (biz_ph_cntpct() * 1000000ULL) / biz_ph_cntfrq();
}

static rt_uint64_t biz_ph_us_since(rt_uint64_t t0)
{
    return ((biz_ph_cntpct() - t0) * 1000000ULL) / biz_ph_cntfrq();
}

void biz_phase_note_query_us(rt_uint64_t us)
{
    (void)us;
}

void biz_phase_note_query_rounds(rt_uint32_t rounds)
{
    s_ph_round_qt_rounds = rounds;
}

void biz_phase_note_task_head(unsigned char cmd, unsigned int tag, rt_uint64_t bd_or_src)
{
    if (s_ph_have_last &&
        s_ph_round_qt_rounds == 1U &&
        cmd == (unsigned char)s_ph_last_cmd &&
        tag == s_ph_last_tag &&
        bd_or_src == s_ph_last_bd)
    {
        s_ph_stale_total++;
    }
    s_ph_last_cmd = cmd;
    s_ph_last_tag = tag;
    s_ph_last_bd = bd_or_src;
    s_ph_have_last = 1;
}

void biz_phase_note_report_us(rt_uint64_t us)
{
    s_ph_round_report_us = us;
}

void biz_phase_note_cmd(unsigned char cmd, rt_uint64_t us)
{
    if (cmd == HP640_CMD_ExecBD)
    {
        s_ph_execbd_us += us;
        s_ph_execbd_n++;
    }
    else if (cmd == HP640_CMD_CRC32)
    {
        s_ph_crc_us += us;
        s_ph_crc_n++;
    }
    else
    {
        s_ph_other_us += us;
        s_ph_other_n++;
    }
}

void biz_phase_pkg_done(rt_uint64_t query_us, rt_uint32_t qt_rounds, rt_uint64_t round_us)
{
    if (query_us > 10000000ULL)
        query_us = 10000000ULL;
    if (round_us > 60000000ULL)
        round_us = 60000000ULL;
    if (qt_rounds > 1000000U)
        qt_rounds = 1000000U;

    s_ph_total_n++;
    s_ph_sum_query_us += query_us;
    s_ph_sum_qt_rounds += qt_rounds;
    s_ph_sum_round_us += round_us;
    s_ph_sum_report_us += s_ph_round_report_us;
}

void biz_phase_reset(void)
{
    s_ph_sum_query_us = 0;
    s_ph_sum_report_us = 0;
    s_ph_sum_round_us = 0;
    s_ph_sum_qt_rounds = 0;
    s_ph_execbd_us = 0;
    s_ph_crc_us = 0;
    s_ph_other_us = 0;
    s_ph_execbd_n = 0;
    s_ph_crc_n = 0;
    s_ph_other_n = 0;
    s_ph_total_n = 0;
    s_ph_stale_total = 0;
    s_ph_have_last = 0;
}

void biz_phase_dump(void)
{
    rt_uint32_t n = s_ph_total_n;

    if (n == 0U)
    {
        rt_kprintf("[phase] no samples yet\n");
        return;
    }

    rt_kprintf("[phase] n=%u stale_rehit=%u\n",
               (unsigned)n, (unsigned)s_ph_stale_total);
    rt_kprintf("[phase] avg_query=%uus avg_qt_rounds=%u avg_report=%uus avg_round=%uus\n",
               (unsigned)(s_ph_sum_query_us / n),
               (unsigned)(s_ph_sum_qt_rounds / n),
               (unsigned)(s_ph_sum_report_us / n),
               (unsigned)(s_ph_sum_round_us / n));
    if (s_ph_execbd_n)
        rt_kprintf("[phase] ExecBD  n=%u avg=%uus\n",
                   (unsigned)s_ph_execbd_n,
                   (unsigned)(s_ph_execbd_us / s_ph_execbd_n));
    if (s_ph_crc_n)
        rt_kprintf("[phase] CRC32   n=%u avg=%uus\n",
                   (unsigned)s_ph_crc_n,
                   (unsigned)(s_ph_crc_us / s_ph_crc_n));
    if (s_ph_other_n)
        rt_kprintf("[phase] other   n=%u avg=%uus\n",
                   (unsigned)s_ph_other_n,
                   (unsigned)(s_ph_other_us / s_ph_other_n));
}
#endif

const char *biz_hp640_cmd_name(unsigned char cmd)
{
    switch (cmd)
    {
    case HP640_CMD_Idle:       return "Idle";
    case HP640_CMD_Load:       return "Load";
    case HP640_CMD_Store:      return "Store";
    case HP640_CMD_ExecBD:     return "ExecBD";
    case HP640_CMD_Write:      return "Write";
    case HP640_CMD_DgbRead:    return "DgbRead";
    case HP640_CMD_Copy:       return "Copy";
    case HP640_CMD_Wait:       return "Wait";
    case HP640_CMD_DgbWait:    return "DgbWait";
    case HP640_CMD_FlashWrite: return "FlashWrite";
    case HP640_CMD_FlashRead:  return "FlashRead";
    case HP640_CMD_Config:     return "Config";
    case HP640_CMD_Delay:      return "Delay";
    case HP640_CMD_PWM:        return "PWM";
    case HP640_CMD_ApuClock:   return "ApuClock";
    case HP640_CMD_CRC32:      return "CRC32";
    case HP640_CMD_APUDebug:   return "APUDebug";
    default:                   return "Cmd";
    }
}

static int biz_task_is_host_upgrade(unsigned char cmd)
{
    return cmd == HP640_CMD_Load || cmd == HP640_CMD_FlashWrite;
}

static void biz_upgrade_fail(int idx, const HP640_Task *task, const char *step, int ret)
{
    BIZ_ERROR("[biz] FAIL step=%s idx=%d cmd=%s(0x%02x) tag=%u ret=%d "
              "src=0x%lx dst_flash=0x%lx size=%u blk=%u\n",
              step, idx, biz_hp640_cmd_name(task->cmd), task->cmd, task->tag, ret,
              (unsigned long)task->src_addr, (unsigned long)task->dst_flash,
              task->size, task->blk_cnt);
}

static int exec_task_load(const HP640_Task *task)
{
    int ret;
    int cnt;
    int is_fifo = (task->flag & HP640_CMD_FIFO) ? 1 : 0;

    if (task->dst_addr & (BIZ_ARCH_DMA_MINALIGN - 1))
        return BIZ_ERR_CLI_PARAM;

    if (task->max_cnt == 0)
    {
        ret = drv_emmc_read_blocks((uint32_t)task->src_addr,
            (uint8_t *)(uintptr_t)task->dst_addr, task->blk_cnt, BIZ_BLK_SIZE);
        if (ret != BIZ_SUCCESS)
            BIZ_ERROR("Load eMMC read fail emmc=0x%x dst=0x%lx blks=%u ret=%d\n",
                      (unsigned)task->src_addr, (unsigned long)task->dst_addr,
                      task->blk_cnt, ret);
        else
            BIZ_DEBUG("[biz] OK Load emmc=0x%x dst=0x%lx blks=%u tag=%u\n",
                     (unsigned)task->src_addr, (unsigned long)task->dst_addr,
                     task->blk_cnt, task->tag);
        return ret;
    }

    for (cnt = 0; cnt < (int)task->blk_cnt - (int)task->max_cnt; cnt += task->max_cnt)
    {
        ret = drv_emmc_read_blocks((uint32_t)(task->src_addr + (is_fifo ? 0 : cnt * BIZ_BLK_SIZE)),
            (uint8_t *)(uintptr_t)(task->dst_addr + cnt * BIZ_BLK_SIZE),
            task->max_cnt, BIZ_BLK_SIZE);
        if (ret != BIZ_SUCCESS)
        {
            BIZ_ERROR("Load eMMC read chunk fail emmc=0x%x dst=0x%lx cnt=%d ret=%d\n",
                      (unsigned)task->src_addr, (unsigned long)task->dst_addr, cnt, ret);
            return ret;
        }
    }

    ret = drv_emmc_read_blocks((uint32_t)(task->src_addr + (is_fifo ? 0 : cnt * BIZ_BLK_SIZE)),
        (uint8_t *)(uintptr_t)(task->dst_addr + cnt * BIZ_BLK_SIZE),
        (uint16_t)(task->blk_cnt - cnt), BIZ_BLK_SIZE);
    if (ret != BIZ_SUCCESS)
        BIZ_ERROR("Load eMMC read tail fail emmc=0x%x dst=0x%lx ret=%d\n",
                  (unsigned)task->src_addr, (unsigned long)task->dst_addr, ret);
    else
        BIZ_DEBUG("[biz] OK Load emmc=0x%x dst=0x%lx blks=%u tag=%u\n",
                 (unsigned)task->src_addr, (unsigned long)task->dst_addr,
                 task->blk_cnt, task->tag);
    return ret;
}

static int exec_task_store(const HP640_Task *task)
{
    int ret;
    int cnt;
    int is_fifo = (task->flag & HP640_CMD_FIFO) ? 1 : 0;

    if (task->src_addr & (BIZ_ARCH_DMA_MINALIGN - 1))
        return BIZ_ERR_CLI_PARAM;

    if (task->max_cnt == 0)
    {
#ifndef BSP_BIZ_SKIP_HOST_DCACHE
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
            (void *)(uintptr_t)task->src_addr, (rt_size_t)task->blk_cnt * BIZ_BLK_SIZE);
#endif
        return drv_emmc_write_blocks((uint32_t)task->dst_addr,
            (uint8_t *)(uintptr_t)task->src_addr, task->blk_cnt, BIZ_BLK_SIZE);
    }

    for (cnt = 0; cnt < (int)task->blk_cnt - (int)task->max_cnt; cnt += task->max_cnt)
    {
#ifndef BSP_BIZ_SKIP_HOST_DCACHE
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
            (void *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
            (rt_size_t)task->max_cnt * BIZ_BLK_SIZE);
#endif
        ret = drv_emmc_write_blocks((uint32_t)(task->dst_addr + (is_fifo ? 0 : cnt * BIZ_BLK_SIZE)),
            (uint8_t *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
            task->max_cnt, BIZ_BLK_SIZE);
        if (ret != BIZ_SUCCESS)
            return ret;
    }

#ifndef BSP_BIZ_SKIP_HOST_DCACHE
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
        (void *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
        (rt_size_t)(task->blk_cnt - cnt) * BIZ_BLK_SIZE);
#endif
    return drv_emmc_write_blocks((uint32_t)(task->dst_addr + (is_fifo ? 0 : cnt * BIZ_BLK_SIZE)),
        (uint8_t *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
        (uint16_t)(task->blk_cnt - cnt), BIZ_BLK_SIZE);
}

static int exec_task_exec_bd(const HP640_Task *task)
{
    BIZ_DEBUG("Task>>>execBD bd_addr=0x%lx batch_cnt=%d batch_size=%d\n",
              (unsigned long)task->bd_addr, task->batch_cnt, task->batch_size);

    if (task->bd_addr & (BIZ_ARCH_DMA_MINALIGN - 1))
        return BIZ_ERR_CLI_PARAM;

    return drv_emmc_exec_bd(task->bd_addr);
}

static int exec_task_write(const HP640_Task *task)
{
    /* Align hp640: SPL_DEBUG("Task>>>write imm_value=… dst_addr=…") */
    BIZ_DEBUG("Task>>>write imm_value=0x%lx dst_addr=0x%lx size=%u\n",
              (unsigned long)task->imm_value, (unsigned long)task->dst_addr,
              task->size);

    switch (task->size)
    {
    case 1:
        mmio_write8(task->dst_addr, (uint8_t)task->imm_value);
        break;
    case 2:
        mmio_write16(task->dst_addr, (uint16_t)task->imm_value);
        break;
    case 4:
        mmio_write32(task->dst_addr, (uint32_t)task->imm_value);
        break;
    case 8:
        mmio_write64(task->dst_addr, (uint64_t)task->imm_value);
        break;
    default:
        BIZ_ERROR("unsupported size of write task (%u); only 1,2,4,8\n",
                  task->size);
        return BIZ_ERR_PRIM_PARAM;
    }
    return BIZ_SUCCESS;
}

static int exec_task_copy(const HP640_Task *task)
{
    rt_memcpy((void *)(uintptr_t)task->dst_addr,
              (const void *)(uintptr_t)task->src_addr, task->size);
    return BIZ_SUCCESS;
}

static int exec_task_wait(const HP640_Task *task)
{
    int times = 0;

    while ((mmio_read32(task->src_addr) & task->mask) != task->value)
    {
        rt_hw_us_delay(10);
        times++;
        if (times >= (int)task->timeout * 100)
            return BIZ_SUCCESS;

        if (biz_emmc_process_heartbeat() != BIZ_SUCCESS)
            return BIZ_ERR_NORMAL;
    }
    return BIZ_SUCCESS;
}

static int exec_task_config(const HP640_Task *task)
{
    /* log_level is at offset 2 in packed HP640_Config (same as hp640). */
    const unsigned int log_level_offset = 2U;
    unsigned long off = task->cfg_offset;
    unsigned int size = task->size;

    BIZ_DEBUG("[biz] Config exec: tag=%u off=%lu size=%u val=0x%lx\n",
              task->tag, off, size, (unsigned long)task->imm_value);

    if (size != 1 && size != 2 && size != 4 && size != 8)
    {
        BIZ_ERROR("[biz] Config bad size=%u (need 1/2/4/8) tag=%u\n",
                  size, task->tag);
        return BIZ_ERR_PRIM_PARAM;
    }
    if (off >= sizeof(biz_config) || size > sizeof(biz_config) - off)
    {
        BIZ_ERROR("[biz] Config bad off=%lu size=%u (cfg sz=%u) tag=%u\n",
                  off, size, (unsigned)sizeof(biz_config), task->tag);
        return BIZ_ERR_PRIM_PARAM;
    }

    switch (size)
    {
    case 1:
        *((uint8_t *)((uint8_t *)&biz_config + off)) = (uint8_t)task->imm_value;
        break;
    case 2:
        *((uint16_t *)((uint8_t *)&biz_config + off)) = (uint16_t)task->imm_value;
        break;
    case 4:
        *((uint32_t *)((uint8_t *)&biz_config + off)) = (uint32_t)task->imm_value;
        break;
    case 8:
        *((uint64_t *)((uint8_t *)&biz_config + off)) = (uint64_t)task->imm_value;
        break;
    default:
        return BIZ_ERR_PRIM_PARAM;
    }

    /* Filter points at biz_config.log_level (hp640 set_loglevel_pt).
     * Host may also write size=4@off=0 covering log_level — pointer sees it. */
    if (off <= log_level_offset && off + size > log_level_offset)
        BIZ_DEBUG("[biz] Config log_level → %u (%s)\n",
                  (unsigned)biz_config.log_level,
                  biz_log_level_name(biz_config.log_level));

    return BIZ_SUCCESS;
}

static int exec_task_delay(const HP640_Task *task)
{
    rt_hw_us_delay((uint32_t)task->microsecond);
    return BIZ_SUCCESS;
}

static int exec_task_flash_read(const HP640_Task *task)
{
    return drv_flash_read((void *)(uintptr_t)task->dst_addr, task->size,
                          (uint32_t)task->src_flash);
}

static int exec_task_flash_write(const HP640_Task *task)
{
#ifdef BIZ_MOD_FLASH_UPGRADE
    char tmp[512];
    int i, j;
    int retry_count = 0;
    uint32_t calculated_crc;
    int ret;

    calculated_crc = biz_crc32_calc(0, (const unsigned char *)task,
                                     sizeof(HP640_Task) - sizeof(task->crc));
    if (calculated_crc != task->crc)
    {
        BIZ_ERROR("[biz] FAIL step=crc tag=%u calc=0x%x expect=0x%x\n",
                  task->tag, calculated_crc, task->crc);
        return BIZ_ERR_PRIM_PARAM;
    }

retry_write:
    /*
     * Load ran on CPU1 (eMMC DMA + invalidate there). Flash writer may be
     * CPU0 worker — invalidate src on the writer path inside drv_flash_write.
     * Before verify on this CPU, invalidate again so compare uses memory.
     */
    ret = drv_flash_write((const void *)(uintptr_t)task->src_addr, task->size,
                          (uint32_t)task->dst_flash);
    if (ret != 0)
    {
        BIZ_ERROR("[biz] FAIL step=flash_write tag=%u flash=0x%lx ret=%d\n",
                  task->tag, (unsigned long)task->dst_flash, ret);
        return ret;
    }

    if (!hp232x_addr_is_normal_nc((const void *)(uintptr_t)task->src_addr,
                                  task->size))
    {
        uintptr_t line = 64;
        uintptr_t s = (uintptr_t)task->src_addr & ~(line - 1U);
        uintptr_t e = ((uintptr_t)task->src_addr + task->size + line - 1U) &
                      ~(line - 1U);

        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)s, (int)(e - s));
    }

    i = 0;
    while (i < task->size)
    {
        if (drv_flash_read(tmp, (int)sizeof(tmp), (uint32_t)task->dst_flash + (uint32_t)i) != 0)
        {
            BIZ_ERROR("[biz] FAIL step=verify_read tag=%u off=%d\n",
                      task->tag, i);
            return BIZ_ERR_FLASH_WRITE_CHECK;
        }

        for (j = 0; j < (int)sizeof(tmp) && i < task->size; j++, i++)
        {
            if (((const char *)(uintptr_t)task->src_addr)[i] != tmp[j])
            {
                if (retry_count < 10)
                {
                    BIZ_WARN("[biz] verify mismatch retry=%d off=%d "
                             "wr=0x%02x rd=0x%02x\n",
                             retry_count + 1, i,
                             ((const unsigned char *)(uintptr_t)task->src_addr)[i],
                             (unsigned char)tmp[j]);
                    retry_count++;
                    goto retry_write;
                }
                BIZ_ERROR("[biz] FAIL step=verify tag=%u off=%d wr=0x%02x rd=0x%02x\n",
                          task->tag, i,
                          ((const unsigned char *)(uintptr_t)task->src_addr)[i],
                          (unsigned char)tmp[j]);
                return BIZ_ERR_FLASH_WRITE_CHECK;
            }
        }
    }
    BIZ_INFO("[biz] OK FlashWrite flash=0x%lx size=%u tag=%u\n",
             (unsigned long)task->dst_flash, task->size, task->tag);
    return BIZ_SUCCESS;
#else
    return drv_flash_write((const void *)(uintptr_t)task->src_addr, task->size,
                           (uint32_t)task->dst_flash);
#endif
}

int biz_emmc_exec_tasks(HP640_Task *task_base, int task_count, int *task_ret)
{
    int ret = BIZ_SUCCESS;
    int idx = 0;
    int has_execbd = 0;
    HP640_Task *task;

    if (!task_base || !task_ret)
        return BIZ_ERR_CLI_PARAM;

    if (!biz_emmc_get_apu_init_flag())
    {
        for (int i = 0; i < task_count; i++)
        {
            if (task_base[i].cmd == HP640_CMD_ExecBD)
            {
                has_execbd = 1;
                break;
            }
        }
        if (has_execbd)
        {
            drv_apu_enable(1);
            biz_emmc_set_apu_init_flag(1);
            BIZ_DEBUG("apu init flag=%d\n", biz_emmc_get_apu_init_flag());
        }
    }

    while (idx < task_count)
    {
        task = task_base + idx;

        if (biz_task_is_host_upgrade(task->cmd))
        {
            /* Load: union holds blk_cnt/max_cnt — do not print as size */
            if (task->cmd == HP640_CMD_Load)
                BIZ_DEBUG("[biz] Load tag=%u blks=%u bytes=%u dst=0x%lx\n",
                         task->tag, task->blk_cnt,
                         (unsigned)task->blk_cnt * BIZ_BLK_SIZE,
                         (unsigned long)task->dst_addr);
            else
                BIZ_INFO("[biz] %s tag=%u size=%u flash=0x%lx\n",
                         biz_hp640_cmd_name(task->cmd), task->tag, task->size,
                         (unsigned long)task->dst_flash);
        }

        if (task->cmd == HP640_CMD_Loop)
        {
            if (task->times != 0)
            {
                task->loop_counter++;
                if (task->loop_counter >= task->times)
                {
                    task->loop_counter = 0;
                    idx++;
                    continue;
                }
            }
            for (idx = 0; idx < task_count; idx++)
            {
                if (task_base[idx].tag == task->loop_tag)
                    break;
            }
            if (idx >= task_count)
            {
                int task_idx = (int)(task - task_base);
                ret = BIZ_ERR_PRIM_LOOP_TAG;
                task_ret[2 * task_idx] = (int)task->tag;
                task_ret[2 * task_idx + 1] = ret;
                break;
            }
            continue;
        }

        {
#ifdef BSP_BIZ_PHASE_STATS
            rt_uint64_t t_cmd = biz_ph_cntpct();
            rt_uint64_t d_us;
#endif
            switch (task->cmd)
            {
            case HP640_CMD_Idle:
                ret = BIZ_SUCCESS;
                break;
            case HP640_CMD_Load:
                ret = exec_task_load(task);
                break;
            case HP640_CMD_Store:
                ret = exec_task_store(task);
                break;
            case HP640_CMD_ExecBD:
                ret = exec_task_exec_bd(task);
                break;
            case HP640_CMD_Write:
                ret = exec_task_write(task);
                break;
            case HP640_CMD_Copy:
                ret = exec_task_copy(task);
                break;
            case HP640_CMD_Wait:
                ret = exec_task_wait(task);
                break;
            case HP640_CMD_DgbWait:
                ret = biz_emmc_process_heartbeat();
                break;
            case HP640_CMD_FlashRead:
                ret = exec_task_flash_read(task);
                break;
            case HP640_CMD_FlashWrite:
                ret = exec_task_flash_write(task);
                break;
            case HP640_CMD_Config:
                ret = exec_task_config(task);
                break;
            case HP640_CMD_Delay:
                ret = exec_task_delay(task);
                break;
#ifdef BIZ_MOD_EXEC_APU
            case HP640_CMD_DgbRead:
                ret = biz_exec_apu_debug_read(task);
                break;
            case HP640_CMD_PWM:
                ret = biz_exec_apu_pwm(task);
                break;
            case HP640_CMD_ApuClock:
                ret = biz_exec_apu_clock(task);
                break;
            case HP640_CMD_APUDebug:
                ret = biz_exec_apu_debug(task);
                break;
#endif
#ifdef BIZ_MOD_EXEC_MISC
            case HP640_CMD_CRC32:
                ret = biz_exec_crc32(task);
                break;
            case HP640_CMD_Timer:
                ret = biz_exec_timer(task, task_base, task_count);
                break;
            case HP640_CMD_SetTimestamp:
                ret = biz_exec_set_timestamp(task);
                break;
#endif
#ifdef BIZ_MOD_EXEC_STRESS
            case HP640_CMD_Stress:
                ret = biz_exec_stress(task);
                break;
#endif
#ifdef BIZ_MOD_EXEC_SELFTEST
            case HP640_CMD_SelfTest:
                ret = biz_exec_self_test();
                break;
#endif
#ifdef BIZ_MOD_PCIE
            case HP640_CMD_PCIeStress:
                ret = biz_exec_pcie_stress(task);
                break;
            case HP640_CMD_PCIeSetup:
                ret = biz_exec_pcie_setup(task);
                break;
#endif
#ifdef BIZ_MOD_EMMC_DLL
            case HP640_CMD_EMMC_DLL_SCAN:
                ret = biz_exec_emmc_dll_scan();
                break;
#endif
            default:
                BIZ_WARN("Unknown task cmd=0x%x\n", task->cmd);
                ret = BIZ_ERR_PRIM_ID;
                break;
            }
#ifdef BSP_BIZ_PHASE_STATS
            d_us = biz_ph_us_since(t_cmd);
            biz_phase_note_cmd(task->cmd, d_us);
#endif
        }

        if (ret != BIZ_SUCCESS)
        {
            if (biz_task_is_host_upgrade(task->cmd))
                biz_upgrade_fail(idx, task, "task_exec", ret);
            biz_mcu_err_post((biz_err_code_t)ret);
        }

        *(unsigned int *)&task_ret[2 * idx] = task->tag;
        task_ret[2 * idx + 1] = ret;

        if (ret != BIZ_SUCCESS)
            return ret;

        if (!(task->flag & HP640_CMD_NOT_END))
            break;

        idx++;
    }

    return ret;
}
