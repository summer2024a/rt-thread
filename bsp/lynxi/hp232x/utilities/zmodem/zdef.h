/*
 * ZMODEM definitions — adapted from RT-Thread utilities/zmodem (v4.0.5)
 * for hp232x memory-sink receive (no DFS).
 */
#ifndef __ZDEF_H__
#define __ZDEF_H__

#include <rtthread.h>
#include <rtdevice.h>

#define ZPAD      '*'
#define ZDLE      030
#define ZDLEE     (ZDLE ^ 0100)
#define ZBIN      'A'
#define ZHEX      'B'
#define ZBIN32    'C'
#define ZBINR32   'D'
#define ZRESC     0176

#define ZRQINIT   0
#define ZRINIT    1
#define ZSINIT    2
#define ZACK      3
#define ZFILE     4
#define ZSKIP     5
#define ZNAK      6
#define ZABORT    7
#define ZFIN      8
#define ZRPOS     9
#define ZDATA     10
#define ZEOF      11
#define ZFERR     12
#define ZCRC      13
#define ZCHALLENGE 14
#define ZCOMPL    15
#define ZCAN      16
#define ZFREECNT  17
#define ZCOMMAND  18

#define ZCRCE     'h'
#define ZCRCG     'i'
#define ZCRCQ     'j'
#define ZCRCW     'k'
#define ZRUB0     'l'
#define ZRUB1     'm'

#define GOTOR     0400
#define GOTCRCE   (ZCRCE | GOTOR)
#define GOTCRCG   (ZCRCG | GOTOR)
#define GOTCRCQ   (ZCRCQ | GOTOR)
#define GOTCRCW   (ZCRCW | GOTOR)
#define GOTCAN    (GOTOR | 030)

#define ZF0       3
#define ZF1       2
#define ZF2       1
#define ZF3       0
#define ZP0       0
#define ZP1       1
#define ZP2       2
#define ZP3       3

#define CANFDX    0x01
#define CANOVIO   0x02
#define CANBRK    0x04
#define CANRLE    0x10
#define CANLZW    0x20
#define CANFC32   0x28
#define ESCCTL    0x64
#define ESC8      0xc8

#define ZATTNLEN  32

#define ENQ     005
#define CAN     ('X' & 037)
#define XOFF    ('s' & 037)
#define XON     ('q' & 037)
#define TIMEOUT (-2)
#define RCDO    (-3)
#define GCOUNT  (-4)
#define ERRORMAX 5

#define BITRATE         115200
#define RX_BUFFER_SIZE  1024

extern char ZF0_CMD;
extern char ZF1_CMD;
extern char ZF2_CMD;
extern char ZF3_CMD;
extern char Attn[ZATTNLEN + 1];

extern rt_uint8_t  Rxframeind;
extern char header_type;
extern rt_uint8_t  rx_header[4];
extern rt_uint8_t  tx_header[4];
extern rt_uint8_t  Txfcs32;
extern rt_uint16_t Rxcount;
extern rt_uint32_t Rxpos;
extern rt_uint32_t Txpos;
extern rt_uint32_t Baudrate;
extern rt_uint32_t Line_left;

struct zmodemf
{
    rt_device_t device;
};
extern struct zmodemf zmodem;

/* Memory receive sink (replaces DFS file descriptor). */
struct zfile
{
    char         name[64];
    rt_uint8_t  *mem;
    rt_uint32_t  mem_cap;
    rt_uint32_t  bytes_total;
    rt_uint32_t  bytes_received;
};

void zinit_parameter(void);
rt_int16_t zget_header(rt_uint8_t *hdr);
void zsend_bin_header(rt_uint8_t type, rt_uint8_t *hdr);
void zsend_hex_header(rt_uint8_t type, rt_uint8_t *hdr);
rt_int16_t zget_data(rt_uint8_t *buf, rt_uint16_t len);
void zsend_bin_data(rt_uint8_t *buf, rt_int16_t len, rt_uint8_t frameend);
void zput_pos(rt_uint32_t pos);
void zget_pos(rt_uint32_t pos);

rt_uint32_t get_device_baud(void);
void zsend_byte(rt_uint16_t c);
void zsend_line(rt_uint16_t c);
rt_int16_t zread_line(rt_uint16_t timeout);
void zsend_break(char *cmd);
void zsend_can(void);

/** Receive one file into mem; returns RT_EOK and *out_len on success. */
rt_err_t zm_recv_to_mem(rt_device_t dev, void *mem, rt_uint32_t cap,
                        rt_uint32_t *out_len);

#endif /* __ZDEF_H__ */
