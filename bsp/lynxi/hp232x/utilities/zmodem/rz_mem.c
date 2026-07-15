/*
 * ZMODEM receive into a memory buffer (flash update scratch).
 * Adapted from RT-Thread utilities/zmodem/rz.c (no DFS).
 */
#include <rtthread.h>
#include <string.h>
#include <stdlib.h>
#include "zdef.h"

static rt_err_t zrec_init(rt_uint8_t *rxbuf, struct zfile *zf);
static rt_err_t zrec_files(struct zfile *zf);
static rt_err_t zrec_file(rt_uint8_t *rxbuf, struct zfile *zf);
static rt_err_t zget_file_info(char *name, struct zfile *zf);
static rt_err_t zrec_file_data(rt_uint8_t *buf, struct zfile *zf);
static rt_err_t zwrite_mem(rt_uint8_t *buf, rt_uint16_t size, struct zfile *zf);
static void zrec_ack_bibi(void);

static rt_err_t zrec_init(rt_uint8_t *rxbuf, struct zfile *zf)
{
    rt_uint8_t err_cnt = 0;
    rt_err_t res = -RT_ERROR;

    (void)zf;

    for (;;)
    {
        zput_pos(0L);
        tx_header[ZF0] = ZF0_CMD;
        tx_header[ZF1] = ZF1_CMD;
        tx_header[ZF2] = ZF2_CMD;
        zsend_hex_header(ZRINIT, tx_header);
again:
        res = zget_header(rx_header);
        switch (res)
        {
        case ZFILE:
            ZF0_CMD = rx_header[ZF0];
            ZF1_CMD = rx_header[ZF1];
            ZF2_CMD = rx_header[ZF2];
            ZF3_CMD = rx_header[ZF3];
            res = zget_data(rxbuf, RX_BUFFER_SIZE);
            if (res == GOTCRCW)
            {
                if ((res = zget_file_info((char *)rxbuf, zf)) != RT_EOK)
                {
                    zsend_hex_header(ZSKIP, tx_header);
                    return res;
                }
                return RT_EOK;
            }
            zsend_hex_header(ZNAK, tx_header);
            goto again;
        case ZSINIT:
            if (zget_data((rt_uint8_t *)Attn, ZATTNLEN) == GOTCRCW)
            {
                zsend_hex_header(ZACK, tx_header);
                goto again;
            }
            zsend_hex_header(ZNAK, tx_header);
            goto again;
        case ZRQINIT:
            continue;
        case ZEOF:
            continue;
        case ZCOMPL:
            goto again;
        case ZFIN:
            zrec_ack_bibi();
            return res;
        default:
            if (++err_cnt > 100)
                return -RT_ERROR;
            continue;
        }
    }
}

static rt_err_t zrec_files(struct zfile *zf)
{
    static rt_uint8_t rxbuf[RX_BUFFER_SIZE];
    rt_err_t res;

    zinit_parameter();
    if ((res = zrec_init(rxbuf, zf)) != RT_EOK)
        return -RT_ERROR;

    res = zrec_file(rxbuf, zf);
    if (res == ZFIN || res == ZCOMPL)
        return RT_EOK;
    if (res == ZCAN)
        return ZCAN;

    zsend_can();
    return res;
}

static rt_err_t zrec_file(rt_uint8_t *rxbuf, struct zfile *zf)
{
    rt_err_t res = -RT_ERROR;
    rt_uint16_t err_cnt = 0;

    do
    {
        zput_pos(zf->bytes_received);
        zsend_hex_header(ZRPOS, tx_header);
again:
        res = zget_header(rx_header);
        switch (res)
        {
        case ZDATA:
            zget_pos(Rxpos);
            if (Rxpos != zf->bytes_received)
            {
                zsend_break(Attn);
                continue;
            }
            err_cnt = 0;
            res = zrec_file_data(rxbuf, zf);
            if (res == -RT_ERROR)
            {
                zsend_break(Attn);
                continue;
            }
            else if (res == GOTCAN)
                return res;
            else
                goto again;
        case ZRPOS:
            zget_pos(Rxpos);
            continue;
        case ZEOF:
            err_cnt = 0;
            zget_pos(Rxpos);
            if (Rxpos != zf->bytes_received)
                continue;
            /* ready for next file / session end */
            return zrec_init(rxbuf, zf);
        case ZFIN:
            zrec_ack_bibi();
            return ZCOMPL;
        case ZCAN:
#ifdef ZDEBUG
            rt_kprintf("ZCAN\n");
#endif
            return res;
        default:
            continue;
        }
    } while (++err_cnt < 100);

    return res;
}

static rt_err_t zget_file_info(char *name, struct zfile *zf)
{
    char *p;
    unsigned long total = 0;

    if (!name || !zf || !zf->mem || zf->mem_cap == 0)
        return -RT_ERROR;

    rt_strncpy(zf->name, name, sizeof(zf->name) - 1);
    zf->name[sizeof(zf->name) - 1] = '\0';

    /* ZFILE info: name\0size[ mtime mode] — avoid sscanf (pulls float scanf). */
    p = name + strlen(name) + 1;
    if (*p)
        total = strtoul(p, RT_NULL, 10);

    if (total == 0)
        total = zf->mem_cap;

    if (total > zf->mem_cap)
    {
        zsend_can();
        return -RT_ERROR;
    }

    zf->bytes_total = (rt_uint32_t)total;
    zf->bytes_received = 0;
    return RT_EOK;
}

static rt_err_t zrec_file_data(rt_uint8_t *buf, struct zfile *zf)
{
    rt_err_t res;

more_data:
    res = zget_data(buf, RX_BUFFER_SIZE);
    switch (res)
    {
    case GOTCRCW:
        if (zwrite_mem(buf, Rxcount, zf) < 0)
            return -RT_ERROR;
        zf->bytes_received += Rxcount;
        zput_pos(zf->bytes_received);
        zsend_line(XON);
        zsend_hex_header(ZACK, tx_header);
        return RT_EOK;
    case GOTCRCQ:
        if (zwrite_mem(buf, Rxcount, zf) < 0)
            return -RT_ERROR;
        zf->bytes_received += Rxcount;
        zput_pos(zf->bytes_received);
        zsend_hex_header(ZACK, tx_header);
        goto more_data;
    case GOTCRCG:
        if (zwrite_mem(buf, Rxcount, zf) < 0)
            return -RT_ERROR;
        zf->bytes_received += Rxcount;
        goto more_data;
    case GOTCRCE:
        if (zwrite_mem(buf, Rxcount, zf) < 0)
            return -RT_ERROR;
        zf->bytes_received += Rxcount;
        return RT_EOK;
    case GOTCAN:
        return res;
    case TIMEOUT:
        return res;
    case -RT_ERROR:
        zsend_break(Attn);
        return res;
    default:
        return res;
    }
}

static rt_err_t zwrite_mem(rt_uint8_t *buf, rt_uint16_t size, struct zfile *zf)
{
    if ((rt_uint32_t)size + zf->bytes_received > zf->mem_cap)
        return -RT_ERROR;
    memcpy(zf->mem + zf->bytes_received, buf, size);
    return (rt_err_t)size;
}

static void zrec_ack_bibi(void)
{
    rt_uint8_t i;

    zput_pos(0L);
    for (i = 0; i < 3; i++)
    {
        zsend_hex_header(ZFIN, tx_header);
        if (zget_header(rx_header) == ZFIN)
        {
            zsend_line('O');
            zsend_line('O');
            break;
        }
    }
}

rt_err_t zm_recv_to_mem(rt_device_t dev, void *mem, rt_uint32_t cap,
                        rt_uint32_t *out_len)
{
    struct zfile zf;
    rt_err_t res;

    if (!dev || !mem || cap == 0)
        return -RT_EINVAL;

    memset(&zf, 0, sizeof(zf));
    zf.mem = (rt_uint8_t *)mem;
    zf.mem_cap = cap;

    zmodem.device = dev;
    Line_left = 0;

    res = zrec_files(&zf);
    if (res == RT_EOK || res == ZCOMPL)
    {
        if (out_len)
            *out_len = zf.bytes_received;
        return RT_EOK;
    }

    if (out_len)
        *out_len = zf.bytes_received;
    return (res < 0) ? res : -RT_ERROR;
}
