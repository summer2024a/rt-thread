/*
 * biz_crc32.c — software CRC32 (IEEE / zlib polynomial), hp640 CRC32 task.
 */
#include <rtthread.h>
#include "biz_exec_handlers.h"

uint32_t biz_crc32_calc(uint32_t crc, const void *buf, rt_size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    rt_size_t i;
    int k;

    crc = crc ^ 0xFFFFFFFFU;
    for (i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}
