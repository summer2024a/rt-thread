/*
 * drv_main.c — Upper-layer status / info only (no hardware init).
 */
#include <rtthread.h>
#include "biz_subsys.h"
#include "biz_log.h"
#include "drv_efuse.h"
#include "drv_flash.h"
#include "biz_ipc.h"
#include "hp_fmt.h"
#ifdef BSP_DRV_PVT_BOOT_SAMPLE
#include "drv_pvt.h"
#endif

static void print_chip_info(void)
{
    unsigned char uuid[33] = {0};
    int chip_type = drv_efuse_check_chip_type();

    if (drv_efuse_get_chip_uuid_simple(uuid) == 0)
        BIZ_INFO("Chip: %s, UUID: %s\n",
                 chip_type == DRV_CHIP_KA200M ? "KA200M" : "KA200", uuid);
    else
        BIZ_WARN("eFuse UUID read failed\n");
}

#ifdef BSP_DRV_PVT_BOOT_SAMPLE
static void pvt_boot_sample(void)
{
    float temp[DRV_PVT_TS_COUNT];
    float volt[DRV_PVT_VM_COUNT];
    char fbuf[24];
    int i;

    if (drv_pvt_read_temperature(temp, 0) == 0)
    {
        for (i = 0; i < DRV_PVT_TS_COUNT; i++)
        {
            if (hp_fmt_pvt_temp_c(fbuf, sizeof(fbuf), temp[i]) < 0)
                continue;
            BIZ_INFO("PVT TS[%d]: %s C\n", i, fbuf);
        }
    }
    else
    {
        BIZ_WARN("PVT temperature read failed\n");
    }

    if (drv_pvt_read_voltage(volt, 0) == 0)
    {
        for (i = 0; i < DRV_PVT_VM_COUNT; i++)
        {
            if (hp_fmt_pvt_volt_v(fbuf, sizeof(fbuf), volt[i]) < 0)
                continue;
            BIZ_INFO("PVT VM[%d]: %s V\n", i, fbuf);
        }
    }
    else
    {
        BIZ_WARN("PVT voltage read failed\n");
    }
}
#endif

static void flash_error_check(void)
{
    uint32_t err_flag = 0;

    if (!drv_flash_is_known())
    {
        HP_LOGI("[drv] app: skip flash error check (unknown JEDEC)\n");
        return;
    }

    if (drv_flash_read(&err_flag, 4, DRV_FLASH_EMMC_ERROR_ADDR) == 0 &&
        err_flag != 0 && err_flag != 0xFFFFFFFFU)
    {
        /* Do not GPIO-pulse here — emmc_biz clears+reports after HS400. */
        BIZ_ERROR("eMMC error flag in flash: 0x%08x (defer to emmc_biz)\n",
                  err_flag);
    }
}

void biz_app_info_show(void)
{
    BIZ_INFO("Log buffer @ 0x%08x (%dKB, preserved=%d)\n",
             (uint32_t)(uintptr_t)&g_log_buffer,
             BIZ_LOG_BUFFER_SIZE / 1024,
             g_log_buffer.header.magic == BIZ_LOG_MAGIC && g_log_buffer.header.total_logs > 0);

    print_chip_info();
#ifdef BSP_DRV_PVT_BOOT_SAMPLE
    pvt_boot_sample();
#endif
    flash_error_check();
}

static int biz_app_info_init(void)
{
#ifdef BSP_BIZ_SKIP_THREADS
    HP_LOGI("[drv] app: deferred (BSP_BIZ_SKIP_THREADS) — skip flash err check\n");
    return 0;
#else
    HP_LOGI("[drv] app: info\n");
    biz_app_info_show();
    HP_LOGI("[drv] app: done\n");
    return 0;
#endif
}
INIT_APP_EXPORT(biz_app_info_init);
