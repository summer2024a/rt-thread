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
#include "tick.h"

extern int biz_emmc_get_apu_init_flag(void);
extern void biz_emmc_set_apu_init_flag(int flag);

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

static inline uint32_t mmio_read32(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

const char *biz_hp640_cmd_name(unsigned char cmd)
{
    switch (cmd)
    {
    case HP640_CMD_Idle:       return "Idle";
    case HP640_CMD_Load:       return "Load";
    case HP640_CMD_Store:      return "Store";
    case HP640_CMD_FlashWrite: return "FlashWrite";
    case HP640_CMD_FlashRead:  return "FlashRead";
    case HP640_CMD_Config:     return "Config";
    case HP640_CMD_Delay:      return "Delay";
    default:                   return "Cmd";
    }
}

static int biz_task_is_host_upgrade(unsigned char cmd)
{
    return cmd == HP640_CMD_Load || cmd == HP640_CMD_FlashWrite;
}

static void biz_upgrade_fail(int idx, const HP640_Task *task, const char *step, int ret)
{
    BIZ_ERROR("[biz][upgrade] FAIL step=%s idx=%d cmd=%s(0x%02x) tag=%u ret=%d "
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
            BIZ_INFO("[biz][upgrade] OK Load emmc=0x%x dst=0x%lx blks=%u tag=%u\n",
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
        BIZ_INFO("[biz][upgrade] OK Load emmc=0x%x dst=0x%lx blks=%u tag=%u\n",
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
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
            (void *)(uintptr_t)task->src_addr, (rt_size_t)task->blk_cnt * BIZ_BLK_SIZE);
        return drv_emmc_write_blocks((uint32_t)task->dst_addr,
            (uint8_t *)(uintptr_t)task->src_addr, task->blk_cnt, BIZ_BLK_SIZE);
    }

    for (cnt = 0; cnt < (int)task->blk_cnt - (int)task->max_cnt; cnt += task->max_cnt)
    {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
            (void *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
            (rt_size_t)task->max_cnt * BIZ_BLK_SIZE);
        ret = drv_emmc_write_blocks((uint32_t)(task->dst_addr + (is_fifo ? 0 : cnt * BIZ_BLK_SIZE)),
            (uint8_t *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
            task->max_cnt, BIZ_BLK_SIZE);
        if (ret != BIZ_SUCCESS)
            return ret;
    }

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
        (void *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
        (rt_size_t)(task->blk_cnt - cnt) * BIZ_BLK_SIZE);
    return drv_emmc_write_blocks((uint32_t)(task->dst_addr + (is_fifo ? 0 : cnt * BIZ_BLK_SIZE)),
        (uint8_t *)(uintptr_t)(task->src_addr + cnt * BIZ_BLK_SIZE),
        (uint16_t)(task->blk_cnt - cnt), BIZ_BLK_SIZE);
}

static int exec_task_exec_bd(const HP640_Task *task)
{
    if (task->bd_addr & (BIZ_ARCH_DMA_MINALIGN - 1))
        return BIZ_ERR_CLI_PARAM;

    return drv_emmc_exec_bd(task->bd_addr);
}

static int exec_task_write(const HP640_Task *task)
{
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
    default:
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
    const unsigned int log_level_offset = 2U;

    switch (task->size)
    {
    case 1:
        *((uint8_t *)((uint8_t *)&biz_config + task->cfg_offset)) = (uint8_t)task->imm_value;
        break;
    case 2:
        *((uint16_t *)((uint8_t *)&biz_config + task->cfg_offset)) = (uint16_t)task->imm_value;
        break;
    case 4:
        *((uint32_t *)((uint8_t *)&biz_config + task->cfg_offset)) = (uint32_t)task->imm_value;
        break;
    default:
        return BIZ_ERR_PRIM_PARAM;
    }

    if (task->cfg_offset == log_level_offset && task->size == 1)
        biz_log_set_level(biz_config.log_level);

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
        BIZ_ERROR("[biz][upgrade] FAIL step=crc tag=%u calc=0x%x expect=0x%x\n",
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
        BIZ_ERROR("[biz][upgrade] FAIL step=flash_write tag=%u flash=0x%lx ret=%d\n",
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
            BIZ_ERROR("[biz][upgrade] FAIL step=verify_read tag=%u off=%d\n",
                      task->tag, i);
            return BIZ_ERR_FLASH_WRITE_CHECK;
        }

        for (j = 0; j < (int)sizeof(tmp) && i < task->size; j++, i++)
        {
            if (((const char *)(uintptr_t)task->src_addr)[i] != tmp[j])
            {
                if (retry_count < 10)
                {
                    BIZ_WARN("[biz][upgrade] verify mismatch retry=%d off=%d "
                             "wr=0x%02x rd=0x%02x\n",
                             retry_count + 1, i,
                             ((const unsigned char *)(uintptr_t)task->src_addr)[i],
                             (unsigned char)tmp[j]);
                    retry_count++;
                    goto retry_write;
                }
                BIZ_ERROR("[biz][upgrade] FAIL step=verify tag=%u off=%d wr=0x%02x rd=0x%02x\n",
                          task->tag, i,
                          ((const unsigned char *)(uintptr_t)task->src_addr)[i],
                          (unsigned char)tmp[j]);
                return BIZ_ERR_FLASH_WRITE_CHECK;
            }
        }
    }
    BIZ_INFO("[biz][upgrade] OK FlashWrite flash=0x%lx size=%u tag=%u\n",
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
        }
    }

    while (idx < task_count)
    {
        task = task_base + idx;

        if (biz_task_is_host_upgrade(task->cmd))
        {
            /* Load: union holds blk_cnt/max_cnt — do not print as size */
            if (task->cmd == HP640_CMD_Load)
                BIZ_INFO("[biz][upgrade] Load tag=%u blks=%u bytes=%u dst=0x%lx\n",
                         task->tag, task->blk_cnt,
                         (unsigned)task->blk_cnt * BIZ_BLK_SIZE,
                         (unsigned long)task->dst_addr);
            else
                BIZ_INFO("[biz][upgrade] %s tag=%u size=%u flash=0x%lx\n",
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
