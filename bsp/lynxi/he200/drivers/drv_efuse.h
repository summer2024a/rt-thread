#ifndef __DRV_EFUSE_H__
#define __DRV_EFUSE_H__

#include <rtthread.h>

#define LYNXI_EFUSE_CHIP_ID_OFFSET    0x0U
#define LYNXI_EFUSE_CHIP_ID_SIZE      16U

rt_err_t lynxi_efuse_read(rt_uint32_t offset, void *buf, rt_size_t size);
rt_err_t lynxi_efuse_read_chip_id(rt_uint8_t *id, rt_size_t size);

#endif /* __DRV_EFUSE_H__ */
