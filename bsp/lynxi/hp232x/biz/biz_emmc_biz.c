/*
 * biz_emmc_biz.c — eMMC business loop (hp640 auto_run).
 */

#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include "board.h"
#include "drv_emmc.h"
#include "biz_config.h"
#include "biz_log.h"
#include "biz_ipc.h"
#include "drv_efuse.h"
#include "drv_pvt.h"
#include "drv_apu.h"
#include "drv_flash.h"
#include "biz_modules.h"
#include "biz_emmc.h"
#include "biz_exec_handlers.h"
#include "drv_i2c.h"
#include "tick.h"

#define CPR_APU_PLL_CONFIG2         0x12500044UL

static HP640_Task s_task_list[HP640_TAKS_PKG_LENGTH]
    __attribute__((aligned(BIZ_ARCH_DMA_MINALIGN)));
static unsigned char s_hb_index = 1;
static rt_tick_t s_last_hb_tick;
static int s_apu_init_flag;
static int g_pvt_init_flag;
static unsigned char s_cached_uuid[33];
static int s_uuid_cached;

static inline uint32_t reg_read32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

/* hp640 read_pvt_ts_max() */
static int read_pvt_ts_max(unsigned char *max_temp)
{
    int ret, i;
    float temp[DRV_PVT_TS_COUNT];

    ret = drv_pvt_read_temperature(temp, (uint32_t)g_pvt_init_flag);
    if (ret != 0)
    {
        BIZ_ERROR("read pvt ts fail (%d)\n", ret);
        return ret;
    }

    *max_temp = 0;
    for (i = 0; i < DRV_PVT_TS_COUNT; i++)
    {
        if (temp[i] < 0.0f)
            continue;
        if (temp[i] > *max_temp)
            *max_temp = (unsigned char)temp[i];
    }

    return 0;
}

/* hp640 read_pvt_apu_vm_avg() */
static int read_pvt_apu_vm_avg(unsigned char *apu_vm)
{
    int ret, i, count;
    float vm[DRV_PVT_APU_VM_COUNT];
    float avg, sum, min, max;

    ret = drv_pvt_read_apu_vm(vm, (uint32_t)g_pvt_init_flag);
    if (ret != 0)
    {
        BIZ_ERROR("read pvt vm fail (%d)\n", ret);
        return ret;
    }

    sum = 0.0f;
    count = 0;
    min = 999.0f;
    max = 0.0f;
    for (i = 2; i < DRV_PVT_APU_VM_COUNT; i++)
    {
        if (vm[i] < 0.1f)
            continue;
        if (vm[i] > 2.0f)
            continue;

        sum += vm[i];
        if (vm[i] > max)
            max = vm[i];
        else if (vm[i] < min)
            min = vm[i];
        count++;
    }
    avg = sum / count;
    *apu_vm = (unsigned char)(avg * 100.0f);

    return 0;
}

/* hp640 get_cached_uuid() */
static int get_cached_uuid(unsigned char uuid[33])
{
    if (!s_uuid_cached)
    {
        int ret = drv_efuse_get_chip_uuid_simple(s_cached_uuid);

        if (ret != 0)
            return ret;
        s_uuid_cached = 1;
    }

    memcpy(uuid, s_cached_uuid, 33);
    return 0;
}

/* hp640 build_heart_beat_package() */
static int build_heart_beat_package(HP640_HeartBeatPackage *heart_beat)
{
    int ret;

    heart_beat->index = s_hb_index++;
    heart_beat->apu = 1;
    heart_beat->ddr_iram = 1;
    heart_beat->emmc = 1;
#ifdef BIZ_MOD_PCIE
    heart_beat->pcie_type = drv_pcie_is_rc() ? 1 : 0;
#else
    heart_beat->pcie_type = 0;
#endif
    heart_beat->pcie_status = 1;
    heart_beat->pcie_link_speed = 1;
    heart_beat->pcie_link_lane = 1;
    heart_beat->i2c_status = 0;
    heart_beat->chip_type = (unsigned char)drv_efuse_check_chip_type();

    ret = read_pvt_ts_max(&heart_beat->temp);
    if (ret != 0)
        return ret;
    ret = read_pvt_apu_vm_avg(&heart_beat->vm);
    if (ret != 0)
        return ret;

    if (heart_beat->temp == 0 || heart_beat->vm == 0)
        g_pvt_init_flag = 0;
    else
        g_pvt_init_flag = 1;

    heart_beat->version = *(unsigned short *)(uintptr_t)(KERNEL_VADDR_START - 2);
#ifdef BIZ_MOD_EMMC_DLL
    heart_beat->dll_offset = biz_emmc_dll_offset_get();
#else
    heart_beat->dll_offset = 0;
#endif
    heart_beat->apu_clk = reg_read32(CPR_APU_PLL_CONFIG2);
    heart_beat->emmc_rx = s_emmc_stats.read_count;
    heart_beat->emmc_tx = s_emmc_stats.write_count;

    return get_cached_uuid(heart_beat->uuid);
}

/* hp640 report_heart_beat() */
int biz_emmc_report_heartbeat(void)
{
    HP640_HeartBeatPackage heart_beat;
    uint32_t emmc_addr;
    uint8_t *heart_beat_buf = drv_emmc_heartbeat_buf();
    int ret;

    memset(&heart_beat, 0, sizeof(heart_beat));
    memset(heart_beat_buf, 0, BIZ_BLK_SIZE);

    ret = build_heart_beat_package(&heart_beat);
    if (ret != 0)
    {
        BIZ_ERROR("build heart-beat failed (%d)\n", ret);
        return ret;
    }

    memcpy(heart_beat_buf, &heart_beat, sizeof(heart_beat));
    drv_emmc_flush_dma_buf(heart_beat_buf, BIZ_BLK_SIZE);

    ret = drv_emmc_try_init(false);
    if (ret != 0)
    {
        BIZ_ERROR("init emmc fail (%d)\n", ret);
        return ret;
    }

    emmc_addr = biz_config.heart_beat_report_base_address;
    emmc_addr |= (0x80U | ((sizeof(heart_beat) / 4U) - 1U)) << 24;

    ret = drv_emmc_write_blocks(emmc_addr, heart_beat_buf, 1, BIZ_BLK_SIZE);
    if (ret != 0)
    {
        BIZ_ERROR("write heart-beat to emmc fail (%d, addr=0x%x)\n", ret, emmc_addr);
        return ret;
    }

    BIZ_DEBUG("Heart-beat reported successfully (index=%d)\n", heart_beat.index);
    return 0;
}

/* Periodic HB only when heart_beat_interval > 0 (Host Config); default 0 = off. */
int biz_emmc_process_heartbeat(void)
{
    if (biz_config.heart_beat_interval <= 0)
        return BIZ_SUCCESS;

    if ((rt_tick_get() - s_last_hb_tick) < rt_tick_from_millisecond(
            (rt_int32_t)biz_config.heart_beat_interval * 1000))
    {
        return BIZ_SUCCESS;
    }

    s_last_hb_tick = rt_tick_get();
    return biz_emmc_report_heartbeat();
}

int biz_emmc_query_task(HP640_Task *task_list, int max_tasks, uint32_t *out_rounds)
{
    uint32_t rounds = 0;
    int ret = BIZ_SUCCESS;
    size_t copy_len;

    if (!task_list || max_tasks <= 0)
        return BIZ_ERR_CLI_PARAM;

    copy_len = (size_t)max_tasks * sizeof(HP640_Task);
    if (copy_len > BIZ_BLK_SIZE)
        copy_len = BIZ_BLK_SIZE;

    /*
     * Align hp640 query_task(): busy-poll until a valid task package is read.
     * Empty slot (cmd==0 && !NOT_END) never returns — keep spinning.
     * eMMC / heartbeat errors: post MCU err and retry immediately (no sleep).
     * Normal return is always BIZ_SUCCESS with a task in task_list.
     */
    while (1)
    {
        do
        {
            ret = BIZ_SUCCESS;
            rounds++;

            ret = biz_emmc_process_heartbeat();
            if (ret != BIZ_SUCCESS)
            {
                BIZ_ERROR("Failed to process heartbeat in query_task (ret=%d)\n", ret);
                break;
            }

            ret = drv_emmc_read_blocks(HP640_QUERY_TASK_ADDR, drv_emmc_query_buf(),
                                       1, BIZ_BLK_SIZE);
            if (ret != BIZ_SUCCESS)
            {
                BIZ_ERROR("Failed to read task from eMMC (ret=%d), retrying...\n", ret);
                biz_mcu_err_post((biz_err_code_t)ret);
                break;
            }

            memcpy(task_list, drv_emmc_query_buf(), copy_len);
        } while (task_list[0].cmd == 0 && !(task_list[0].flag & HP640_CMD_NOT_END));

        if (ret == BIZ_SUCCESS)
            break;
    }

    if (out_rounds)
        *out_rounds = rounds;

    /* Hot path (ExecBD loop): DEBUG only — INFO+UART can double round-trip latency. */
    if (task_list[0].cmd != 0)
    {
        BIZ_DEBUG("[biz] task received cmd=0x%x flag=0x%x tag=%u\n",
                  task_list[0].cmd, task_list[0].flag, task_list[0].tag);
        if (task_list[0].cmd == HP640_CMD_Config)
        {
            BIZ_DEBUG("[biz] Config raw: flag=0x%x size=%u imm=0x%lx off=%lu tag=%u\n",
                      task_list[0].flag, task_list[0].size,
                      (unsigned long)task_list[0].imm_value,
                      (unsigned long)task_list[0].cfg_offset,
                      task_list[0].tag);
        }
    }

    return ret;
}

int biz_emmc_report_task_result(int *task_ret, unsigned int len)
{
    uint32_t emmc_addr;
    int i;
    int need_report = 0;
    int ret = BIZ_SUCCESS;

    if (biz_config.task_result_report_base_address == 0)
        return BIZ_SUCCESS;

    for (i = 0; i < (int)HP640_TAKS_PKG_LENGTH; i++)
    {
        if (s_task_list[i].flag & HP640_CMD_ERR_CODE)
        {
            need_report = 1;
            break;
        }
        if (!(s_task_list[i].flag & HP640_CMD_NOT_END))
            break;
    }

    if (!need_report)
        return BIZ_SUCCESS;

    emmc_addr = biz_config.task_result_report_base_address;
    emmc_addr |= (0x80U | (len / 4U - 1U)) << 24;

#ifndef BSP_BIZ_SKIP_HOST_DCACHE
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, task_ret, len * sizeof(int));
#endif

    ret = drv_emmc_write_blocks(emmc_addr, (uint8_t *)task_ret, 1, BIZ_BLK_SIZE);
    if (ret != BIZ_SUCCESS)
        biz_mcu_err_post((biz_err_code_t)ret);

    return ret;
}

static void check_emmc_error_from_flash(void)
{
    uint32_t err_flag = 0;

    if (!drv_flash_is_known())
        return;

    if (drv_flash_read(&err_flag, 4, DRV_FLASH_EMMC_ERROR_ADDR) == 0 &&
        err_flag != 0 && err_flag != 0xFFFFFFFFU)
    {
        BIZ_ERROR("eMMC error flag in flash: 0x%08x\n", err_flag);
        biz_mcu_err_post(BIZ_ERR_EMMC_DATA_CRC);
    }
}

/* hp640 auto_run() */
void biz_emmc_biz_entry(void *param)
{
    static int s_task_ret[HP640_TAKS_PKG_LENGTH * 2];
    int ret;
    int first_loop = 1;

    (void)param;

    check_emmc_error_from_flash();

#ifdef BIZ_MOD_EMMC_DLL
    biz_emmc_dll_boot_init();
#endif

    ret = drv_emmc_try_init(true);
    if (ret != BIZ_SUCCESS)
    {
        BIZ_ERROR("init emmc fail (%d)\n", ret);
        return;
    }

    ret = biz_emmc_report_heartbeat();
    if (ret != BIZ_SUCCESS)
    {
        BIZ_ERROR("Failed to report initial heart-beat (%d), system cannot continue\n", ret);
        return;
    }
    /* Default heart_beat_interval=0: no further HB in query_task. */

    BIZ_INFO("Entering main task processing loop\n");

#ifdef BSP_BIZ_HOTPATH_NO_TICK_IPI
    /* After HB/init: quiet local tick + SGIs so fpfifo hot path matches SPL. */
    hp232x_biz_hotpath_irq_quiet();
#endif

    while (1)
    {
        uint32_t qt_rounds = 0;
#ifdef BSP_BIZ_PHASE_STATS
        rt_uint64_t ph_round_t0, ph_query_us, ph_round_us, t0, t1, frq, us;
#endif

        if (first_loop)
            BIZ_DEBUG("Querying tasks from eMMC\n");

#ifdef BSP_BIZ_PHASE_STATS
        /* Wall-clock round — same as hp640 auto_run phase2. */
        ph_round_t0 = biz_phase_now_us();
#endif

        /* query_task busy-polls until success; ret is always 0 here (hp640 auto_run). */
        ret = biz_emmc_query_task(s_task_list, HP640_TAKS_PKG_LENGTH, &qt_rounds);

#ifdef BSP_BIZ_PHASE_STATS
        ph_query_us = biz_phase_now_us() - ph_round_t0;
        biz_phase_note_query_us(ph_query_us);
        biz_phase_note_query_rounds(qt_rounds);
        if (s_task_list[0].cmd == HP640_CMD_ExecBD)
            biz_phase_note_task_head(s_task_list[0].cmd, s_task_list[0].tag,
                                     s_task_list[0].bd_addr);
        else
            biz_phase_note_task_head(s_task_list[0].cmd, s_task_list[0].tag,
                                     s_task_list[0].src_addr);
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
        if (frq == 0)
            frq = 31250000ULL;
#endif

        if (s_task_list[0].cmd == HP640_CMD_Load)
        {
            BIZ_DEBUG("[biz] host task pkg: Load tag=%u blks=%u -> 0x%lx\n",
                     s_task_list[0].tag, s_task_list[0].blk_cnt,
                     (unsigned long)s_task_list[0].dst_addr);
        }
        else if (s_task_list[0].cmd == HP640_CMD_Config)
        {
            BIZ_DEBUG("[biz] host task pkg: Config tag=%u offset=%lu size=%u val=0x%lx\n",
                     s_task_list[0].tag,
                     (unsigned long)s_task_list[0].cfg_offset,
                     s_task_list[0].size,
                     (unsigned long)s_task_list[0].imm_value);
        }

        if (first_loop)
            BIZ_DEBUG("Tasks queried (rounds=%u)\n", qt_rounds);

        memset(s_task_ret, 0, sizeof(s_task_ret));
        ret = biz_emmc_exec_tasks(s_task_list, HP640_TAKS_PKG_LENGTH, s_task_ret);
        if (ret != BIZ_SUCCESS)
        {
            BIZ_ERROR("[biz] exec_tasks fail ret=%d cmd=%s(0x%02x) tag=%u\n",
                      ret, biz_hp640_cmd_name(s_task_list[0].cmd), s_task_list[0].cmd,
                      s_task_list[0].tag);
            drv_apu_enable(0);
            s_apu_init_flag = 0;
        }

        if (!biz_config.unlimited_apu_task && s_apu_init_flag)
        {
            drv_apu_enable(0);
            s_apu_init_flag = 0;
        }

#ifdef BSP_BIZ_PHASE_STATS
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t1));
        us = ((t1 - t0) * 1000000ULL) / frq;
        t0 = t1;
        (void)us;
#endif

        biz_emmc_report_task_result(s_task_ret, sizeof(s_task_ret) / sizeof(s_task_ret[0]));

#ifdef BSP_BIZ_PHASE_STATS
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t1));
        us = ((t1 - t0) * 1000000ULL) / frq;
        biz_phase_note_report_us(us);
        ph_round_us = biz_phase_now_us() - ph_round_t0;
        biz_phase_pkg_done(ph_query_us, qt_rounds, ph_round_us);
#endif
        first_loop = 0;
    }
}

int biz_emmc_get_apu_init_flag(void)
{
    return s_apu_init_flag;
}

void biz_emmc_set_apu_init_flag(int flag)
{
    s_apu_init_flag = flag;
}
