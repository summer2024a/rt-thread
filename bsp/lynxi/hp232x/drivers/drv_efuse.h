/*
 * drv_efuse.h — KA200 eFuse driver for RT-Thread HP232x BSP.
 * Ported from hp640_arm/common/lx_common/lx_efuse.c.
 */

#ifndef DRV_EFUSE_H__
#define DRV_EFUSE_H__

#include <stdint.h>

#define EFUSE_BASE_ADDR                 0x12300000UL
#define EFUSE_STATUS_REG_OFFSET         0x3C
#define EFUSE_READ_MASK_REG_OFFSET      0x40
#define EFUSE_OPT_PARTION_START_OFFSET  0x2000

#define EFUSE_STATUS(val)               (0x3u & ((val) >> 2))

#define KA200_M_FLAG_1                  0x360CA0CBu
#define KA200_M_FLAG_2                  0x00026AD0u
#define EFUSE_CHIP_DEFINE_BLK44         44
#define EFUSE_CHIP_DEFINE_BLK45         45
#define EFUSE_UUID_OFFSET               24

typedef enum {
    DRV_EFUSE_IDLE = 0,
    DRV_EFUSE_PROGRAMING = 1,
    DRV_EFUSE_READ = 2,
    DRV_EFUSE_RESERVED = 3,
} drv_efuse_status_t;

typedef enum {
    DRV_CHIP_KA200 = 0,
    DRV_CHIP_KA200M = 1,
} drv_chip_type_t;

int drv_efuse_read_32bit(uint32_t offset, uint32_t *regval);
int drv_efuse_check_chip_type(void);
int drv_efuse_get_chip_uuid(unsigned char uuidstr[64]);
int drv_efuse_get_chip_uuid_simple(unsigned char uuidstr[33]);

#endif /* DRV_EFUSE_H__ */
