/*
 * drv_efuse.c — KA200 eFuse driver for RT-Thread HP232x BSP.
 * Ported from hp640_arm/common/lx_common/lx_efuse.c.
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include "drv_efuse.h"
#include "tick.h"

static uint32_t efuse_readl(uint32_t offset)
{
    return *(volatile uint32_t *)(EFUSE_BASE_ADDR + offset);
}

static void efuse_trigger_read(uint32_t offset)
{
    (void)efuse_readl(EFUSE_OPT_PARTION_START_OFFSET + 4u * offset);
}

static drv_efuse_status_t efuse_get_status(void)
{
    return (drv_efuse_status_t)EFUSE_STATUS(efuse_readl(EFUSE_STATUS_REG_OFFSET));
}

int drv_efuse_read_32bit(uint32_t offset, uint32_t *regval)
{
    uint32_t val;
    int timeout;

    if (!regval)
        return -1;

    timeout = 100000;
    while (efuse_get_status() != DRV_EFUSE_IDLE)
    {
        rt_hw_us_delay(10);
        if (--timeout == 0)
            return -1;
    }

    efuse_trigger_read(offset);

    timeout = 100000;
    while (efuse_get_status() == DRV_EFUSE_READ)
    {
        rt_hw_us_delay(10);
        if (--timeout == 0)
            return -1;
    }

    val = efuse_readl(EFUSE_READ_MASK_REG_OFFSET);
    *regval = val;
    return 0;
}

int drv_efuse_check_chip_type(void)
{
    uint32_t blk44 = 0;
    uint32_t blk45 = 0;

    if (drv_efuse_read_32bit(EFUSE_CHIP_DEFINE_BLK44, &blk44) != 0 ||
        drv_efuse_read_32bit(EFUSE_CHIP_DEFINE_BLK45, &blk45) != 0)
        return DRV_CHIP_KA200;

    if (blk44 == KA200_M_FLAG_1 && blk45 == KA200_M_FLAG_2)
        return DRV_CHIP_KA200M;

    return DRV_CHIP_KA200;
}

int drv_efuse_get_chip_uuid(unsigned char uuidstr[64])
{
    uint32_t uuid_data[4] = {0};
    unsigned char *data = (unsigned char *)uuid_data;
    int i = 0;
    int j = 0;
    int len = 0;

    if (!uuidstr)
        return -1;

    for (i = 0; i < 4; i++)
    {
        if (drv_efuse_read_32bit(EFUSE_UUID_OFFSET + i, &uuid_data[i]) != 0)
            return -1;
    }

    i = j = 0;
    while (len < 16)
    {
        int k;

        for (k = 0; k < 4; k++)
        {
            if (len + k < 16)
            {
                unsigned char byte_val = data[len + k];
                int cnt;

                uuidstr[i++] = "0123456789abcdef"[byte_val & 0xf];
                uuidstr[i++] = "0123456789abcdef"[(byte_val >> 4) & 0xf];
                j += 2;
                cnt = j / 4;
                if (!(j & 0x3) && (cnt > 1) && (cnt < 6))
                    uuidstr[i++] = '-';
            }
        }
        len += 4;
    }

    uuidstr[i] = '\0';
    return 0;
}

int drv_efuse_get_chip_uuid_simple(unsigned char uuidstr[33])
{
    uint32_t uuid_data[4] = {0};
    unsigned char *data = (unsigned char *)uuid_data;
    int i;
    int str_idx = 0;

    if (!uuidstr)
        return -1;

    for (i = 0; i < 4; i++)
    {
        if (drv_efuse_read_32bit(EFUSE_UUID_OFFSET + i, &uuid_data[i]) != 0)
            return -1;
    }

    for (i = 0; i < 16; i++)
    {
        sprintf((char *)&uuidstr[str_idx], "%02x", data[i]);
        str_idx += 2;
    }

    uuidstr[32] = '\0';
    return 0;
}
