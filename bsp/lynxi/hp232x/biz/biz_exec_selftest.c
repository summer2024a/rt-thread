/*
 * biz_exec_selftest.c — HP640 self-test + eMMC report.
 *
 * DDR/IRAM test uses IRAM1 low 256KB @ 0x100000000 (MMU-mapped, not linked).
 * Code/BSS/heap stay in IRAM1 high 256KB per board.h layout.
 */
#include "biz_modules.h"

#ifdef BIZ_MOD_EXEC_SELFTEST

#include <string.h>
#include "board.h"
#include "biz_exec_handlers.h"
#include "biz_config.h"
#include "drv_emmc.h"
#include "drv_flash.h"
#include "drv_apu.h"
#include "biz_ipc.h"
#include "biz_log.h"
#include <rthw.h>

/* hp640 spl_cmd.c: DDR_IRAM_ADDR, test_ddr_iram len=256*1024 */
#define BIZ_SELFTEST_IRAM_ADDR      IRAM1_START
#define BIZ_SELFTEST_IRAM_SIZE      IRAM1_RESERVED_SIZE
#define BIZ_SELFTEST_FLASH_READ_LEN (2 * 1024)

static int s_ddr_test_done;
static int s_ddr_test_ok;

static int test_ddr_iram(void)
{
    unsigned int i;
    uint8_t *data = (uint8_t *)(uintptr_t)BIZ_SELFTEST_IRAM_ADDR;

    if (s_ddr_test_done)
        return s_ddr_test_ok ? 0 : -1;

    s_ddr_test_done = 1;

    for (i = 0; i < BIZ_SELFTEST_IRAM_SIZE; i++)
        data[i] = (uint8_t)i;

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, data, BIZ_SELFTEST_IRAM_SIZE);
    rt_hw_barrier(dsb, sy);

    for (i = 0; i < BIZ_SELFTEST_IRAM_SIZE; i++)
    {
        if (data[i] != (uint8_t)i)
        {
            BIZ_ERROR("selftest IRAM1[%u]=0x%02x expect 0x%02x\n",
                      i, data[i], (uint8_t)i);
            s_ddr_test_ok = 0;
            return -1;
        }
    }

    s_ddr_test_ok = 1;
    return 0;
}

static int test_apu_cr(void)
{
    int ret;

    drv_apu_enable(1);
    drv_apu_reset();
    ret = (drv_apu_read_status() & 0x01) != 0x01;
    drv_apu_enable(0);
    return ret;
}

static int test_spi_flash(void)
{
    void *buf = (void *)(uintptr_t)BIZ_SELFTEST_IRAM_ADDR;

    return drv_flash_read(buf, BIZ_SELFTEST_FLASH_READ_LEN, 0);
}

int biz_self_test_run(HP640_SelfTestReport *report)
{
    if (!report)
        return BIZ_ERR_CLI_PARAM;

    memset(report, 0, sizeof(*report));
    report->major_version = 1;
    report->minor_version = 0;
    report->ddr_iram = test_ddr_iram() ? 0 : 1;
    report->apu_cr = test_apu_cr() ? 0 : 1;
    report->spi_flash = test_spi_flash() ? 0 : 1;
    return BIZ_SUCCESS;
}

int biz_self_test_report(void)
{
    HP640_SelfTestReport report;
    uint32_t emmc_addr;
    int ret;

    ret = biz_self_test_run(&report);
    if (ret != BIZ_SUCCESS)
        return ret;

    ret = drv_emmc_try_init(false);
    if (ret != BIZ_SUCCESS)
        return ret;

    emmc_addr = biz_config.self_test_report_base_address;
    emmc_addr |= (0x80U | (sizeof(report) / 4U - 1U)) << 24;

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &report, sizeof(report));
    ret = drv_emmc_write_blocks(emmc_addr, (uint8_t *)&report, 1, BIZ_BLK_SIZE);
    if (ret != BIZ_SUCCESS)
        biz_mcu_err_post((biz_err_code_t)ret);

    return ret;
}

int biz_exec_self_test(void)
{
    return biz_self_test_report();
}

#endif /* BIZ_MOD_EXEC_SELFTEST */
