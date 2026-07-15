/*
 * biz_emmc_dll.c — eMMC DLL offset scan (hp640 sdhci_scan_dll_offset port).
 */
#include "biz_modules.h"

#ifdef BIZ_MOD_EMMC_DLL

#include <string.h>
#include "biz_exec_handlers.h"
#include "drv_emmc.h"
#include "drv_flash.h"
#include "biz_ipc.h"
#include "biz_log.h"
#include "biz_host_proto.h"

#ifndef CONFIG_EMMC_BASE_ADDR
#define CONFIG_EMMC_BASE_ADDR 0x10040000ULL
#endif

#define EMMC_BASE               ((uintptr_t)CONFIG_EMMC_BASE_ADDR)
#define EMMC_PHY_OFFSET         0x300U
#define DLL_CTRL                0x24U
#define DLL_OFFST               0x29U
#define DLLDL_CNFG              0x28U
#define DLLDBG_MLKDC            0x2cU

#define EMMC_DLL_TEST_ADDR      0x0FF400400UL

static uint8_t s_dll_offset = 0xFFU;

static inline void emmc_phy_writeb(uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(EMMC_BASE + EMMC_PHY_OFFSET + off) = val;
}

static inline void emmc_phy_writew(uint32_t off, uint16_t val)
{
    *(volatile uint16_t *)(EMMC_BASE + EMMC_PHY_OFFSET + off) = val;
}

static inline uint8_t emmc_phy_readb(uint32_t off)
{
    return *(volatile uint8_t *)(EMMC_BASE + EMMC_PHY_OFFSET + off);
}

static void emmc_dll_apply(uint8_t value)
{
    emmc_phy_writew(DLL_CTRL, 0x2);
    emmc_phy_writew(DLL_CTRL, 0x6);
    emmc_phy_writeb(DLLDL_CNFG, 0x60);
    emmc_phy_writeb(DLL_OFFST, value);
    emmc_phy_writew(DLL_CTRL, 0x3);
}

uint8_t biz_emmc_dll_offset_get(void)
{
    return s_dll_offset;
}

static int emmc_dll_load_from_flash(uint8_t *value)
{
    uint8_t v;

    if (!drv_flash_is_known())
        return -1;

    if (drv_flash_read(&v, 1, BIZ_FLASH_DLL_OFFSET_ADDR) != 0)
        return -1;

    if (v != 0 && v != 0xFFU)
    {
        *value = v;
        emmc_dll_apply(v);
        s_dll_offset = v;
        BIZ_INFO("eMMC DLL from flash: 0x%02x\n", v);
        return 0;
    }
    return -1;
}

void biz_emmc_dll_boot_init(void)
{
    uint8_t v = 0xFFU;

    (void)emmc_dll_load_from_flash(&v);
}

int biz_exec_emmc_dll_scan(void)
{
    static uint8_t s_scan_buf[BIZ_BLK_SIZE] __attribute__((aligned(64)));
    int success[128];
    int success_count = 0;
    int max_start = 0, max_len = 0, cur_start = 0, cur_len = 1;
    int ret;
    unsigned int i;

    if (emmc_dll_load_from_flash(&s_dll_offset) == 0)
        return BIZ_SUCCESS;

    if (drv_emmc_try_init(true) != BIZ_SUCCESS)
        return BIZ_ERR_EMMC_INIT;

    BIZ_INFO("eMMC DLL scan start\n");

    for (i = 0; i < 64; i++)
    {
        emmc_phy_writew(DLL_CTRL, 0x2);
        emmc_phy_writew(DLL_CTRL, 0x6);
        emmc_phy_writeb(DLLDL_CNFG, 0x60);
        emmc_phy_writeb(DLL_OFFST, (uint8_t)i);
        emmc_phy_writew(DLL_CTRL, 0x2);

        ret = drv_emmc_read_blocks((uint32_t)EMMC_DLL_TEST_ADDR, s_scan_buf, 1, BIZ_BLK_SIZE);
        if (ret == BIZ_SUCCESS)
            success[success_count++] = (int)i;
    }

    if (success_count == 0)
    {
        biz_mcu_err_post(BIZ_ERR_EMMC_DLL_SCAN_FAILED);
        return BIZ_ERR_EMMC_DLL_SCAN_FAILED;
    }

    for (int j = 1; j < success_count; j++)
    {
        if (success[j] == success[j - 1] + 1)
            cur_len++;
        else
        {
            if (cur_len > max_len)
            {
                max_len = cur_len;
                max_start = cur_start;
            }
            cur_start = j;
            cur_len = 1;
        }
    }
    if (cur_len > max_len)
    {
        max_len = cur_len;
        max_start = cur_start;
    }

    {
        int mid = max_start + max_len / 2;
        int mlkdc = emmc_phy_readb(DLLDBG_MLKDC);
        int good = success[mid] - (mlkdc / 4);

        if (good < 0)
            good += 128;

        s_dll_offset = (uint8_t)good;
        emmc_dll_apply((uint8_t)good);
        drv_flash_write(&s_dll_offset, 1, BIZ_FLASH_DLL_OFFSET_ADDR);
        BIZ_INFO("eMMC DLL scan OK offset=0x%02x\n", s_dll_offset);
    }

    return BIZ_SUCCESS;
}

#endif /* BIZ_MOD_EMMC_DLL */
