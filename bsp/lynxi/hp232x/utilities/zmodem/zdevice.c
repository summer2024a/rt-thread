/*
 * ZMODEM console I/O — hp232x (no DFS / shell dependency).
 * timeout arg matches upstream calls (≈100 ticks / ~1s at 100Hz).
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "zdef.h"

struct zmodemf zmodem;

rt_uint32_t Line_left = 0;
rt_uint32_t Baudrate = BITRATE;

rt_uint32_t get_device_baud(void)
{
    return Baudrate;
}

void zsend_byte(rt_uint16_t ch)
{
    rt_uint8_t c = (rt_uint8_t)(ch & 0xff);
    if (zmodem.device)
        rt_device_write(zmodem.device, 0, &c, 1);
}

void zsend_line(rt_uint16_t c)
{
    zsend_byte(c);
}

rt_int16_t zread_line(rt_uint16_t timeout)
{
    rt_uint8_t ch;
    rt_size_t n;
    rt_tick_t start;
    rt_tick_t wait;

    if (!zmodem.device)
        return TIMEOUT;

    /* Call sites pass ~100; treat as ms*10 (~1s) like classic tenth-seconds. */
    wait = rt_tick_from_millisecond(timeout ? (rt_int32_t)timeout * 10 : 1000);
    start = rt_tick_get();

    while ((rt_tick_get() - start) < wait)
    {
        n = rt_device_read(zmodem.device, 0, &ch, 1);
        if (n == 1)
            return (rt_int16_t)(ch & 0xff);
        rt_thread_mdelay(1);
    }

    return TIMEOUT;
}

void zsend_break(char *cmd)
{
    if (!cmd)
        return;

    for (; *cmd; cmd++)
    {
        switch (*cmd)
        {
        case '\336':
            continue;
        case '\335':
            rt_thread_mdelay(1000);
            continue;
        default:
            zsend_line((rt_uint16_t)*cmd);
            break;
        }
    }
}

void zsend_can(void)
{
    static char cmd[] = {24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 0};

    zsend_break(cmd);
    Line_left = 0;
}
