#include <rtthread.h>
#include "lynxi.h"
#include "drv_efuse.h"

#ifdef BSP_USING_EFUSE

#define LYNXI_EFUSE_SIZE     0x200U

rt_inline rt_uint32_t efuse_read32(rt_uint32_t off)
{
    return *(volatile rt_uint32_t *)(rt_uintptr_t)(EFUSE_BASE + off);
}

static rt_err_t efuse_copy_out(rt_uint32_t offset, rt_uint8_t *buf, rt_size_t size)
{
    rt_uint32_t aligned_off;
    rt_uint32_t head_skip;
    rt_uint32_t word;
    rt_size_t copied = 0;

    aligned_off = offset & ~0x3U;
    head_skip = offset & 0x3U;

    if (head_skip)
    {
        word = efuse_read32(aligned_off);
        while ((head_skip < 4U) && (copied < size))
        {
            buf[copied++] = (rt_uint8_t)((word >> (head_skip * 8U)) & 0xffU);
            head_skip++;
        }
        aligned_off += 4U;
    }

    while ((size - copied) >= 4U)
    {
        word = efuse_read32(aligned_off);
        buf[copied++] = (rt_uint8_t)(word & 0xffU);
        buf[copied++] = (rt_uint8_t)((word >> 8) & 0xffU);
        buf[copied++] = (rt_uint8_t)((word >> 16) & 0xffU);
        buf[copied++] = (rt_uint8_t)((word >> 24) & 0xffU);
        aligned_off += 4U;
    }

    if (copied < size)
    {
        word = efuse_read32(aligned_off);
        while (copied < size)
        {
            buf[copied++] = (rt_uint8_t)(word & 0xffU);
            word >>= 8;
        }
    }

    return RT_EOK;
}

rt_err_t lynxi_efuse_read(rt_uint32_t offset, void *buf, rt_size_t size)
{
    if ((buf == RT_NULL) || (size == 0))
    {
        return -RT_EINVAL;
    }

    if ((offset >= LYNXI_EFUSE_SIZE) || (size > (LYNXI_EFUSE_SIZE - offset)))
    {
        return -RT_EINVAL;
    }

    return efuse_copy_out(offset, (rt_uint8_t *)buf, size);
}

rt_err_t lynxi_efuse_read_chip_id(rt_uint8_t *id, rt_size_t size)
{
    if ((id == RT_NULL) || (size != LYNXI_EFUSE_CHIP_ID_SIZE))
    {
        return -RT_EINVAL;
    }

    return lynxi_efuse_read(LYNXI_EFUSE_CHIP_ID_OFFSET, id, size);
}

#ifdef RT_USING_FINSH
#include <finsh.h>
static void efuse_dump(int argc, char **argv)
{
    rt_uint32_t i;
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    for (i = 0; i < LYNXI_EFUSE_SIZE; i += 4)
    {
        rt_uint32_t v = efuse_read32(i);
        rt_kprintf("efuse[%02x] = 0x%08x\n", i, v);
    }
}
MSH_CMD_EXPORT(efuse_dump, dump efuse raw registers);

static void efuse_chip_id(int argc, char **argv)
{
    rt_uint8_t id[LYNXI_EFUSE_CHIP_ID_SIZE];
    rt_size_t i;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    if (lynxi_efuse_read_chip_id(id, LYNXI_EFUSE_CHIP_ID_SIZE) != RT_EOK)
    {
        rt_kprintf("efuse_chip_id: read failed\n");
        return;
    }
    rt_kprintf("efuse chip id:");
    for (i = 0; i < LYNXI_EFUSE_CHIP_ID_SIZE; i++)
    {
        rt_kprintf(" %02x", id[i]);
    }
    rt_kprintf("\n");
}
MSH_CMD_EXPORT(efuse_chip_id, print 16-byte efuse chip id);
#endif

#endif /* BSP_USING_EFUSE */
