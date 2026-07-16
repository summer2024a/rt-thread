/*
 * biz_log.c — persistent log ring @ 0x100050000 + RT-Thread console capture.
 */

#include "biz_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

biz_log_buffer_t g_log_buffer __attribute__((section(".bss.noclean.log_ring"), aligned(8)));

#ifdef BSP_BIZ_LOG_BOOT_INFO
unsigned char s_log_level = BIZ_LOG_LEVEL_INFO;
#else
unsigned char s_log_level = BIZ_LOG_LEVEL_WARN;
#endif

static volatile rt_uint8_t s_log_ready;
static volatile rt_uint8_t s_in_biz_log_output;

static int biz_log_ring_write(const char *log, int len, int is_error)
{
    biz_log_buffer_t *buf = &g_log_buffer;
    int data_size;
    int write_len;

    if (!log || len <= 0 || buf->header.magic != BIZ_LOG_MAGIC || buf->header.is_frozen)
        return 0;

    if (buf->header.buffer_size != BIZ_LOG_BUFFER_SIZE ||
        buf->header.write_offset >=
            (BIZ_LOG_BUFFER_SIZE - sizeof(biz_log_header_t)))
        return 0;

    data_size = (int)(buf->header.buffer_size - sizeof(biz_log_header_t));
    write_len = (len > BIZ_LOG_ENTRY_MAX_LEN) ? BIZ_LOG_ENTRY_MAX_LEN : len;

    if (buf->header.write_offset + write_len > (uint32_t)data_size)
    {
        if (buf->header.has_error)
        {
            buf->header.is_frozen = 1;
            return 0;
        }
        buf->header.write_offset = 0;
        buf->header.wrap_count++;
    }

    memcpy(&buf->data[buf->header.write_offset], log, write_len);
    if (is_error)
        buf->header.has_error = 1;

    buf->header.write_offset += (uint32_t)write_len;
    buf->header.total_logs++;
    return write_len;
}

void biz_log_init(void)
{
    biz_log_buffer_t *buf = &g_log_buffer;

    if (buf->header.magic != BIZ_LOG_MAGIC ||
        buf->header.version != 1 ||
        buf->header.buffer_size != BIZ_LOG_BUFFER_SIZE)
    {
        memset(buf, 0, sizeof(*buf));
        buf->header.magic = BIZ_LOG_MAGIC;
        buf->header.version = 1;
        buf->header.buffer_size = BIZ_LOG_BUFFER_SIZE;
    }

    s_log_level = BIZ_LOG_LEVEL_WARN;
#ifdef BSP_BIZ_LOG_BOOT_INFO
    s_log_level = BIZ_LOG_LEVEL_INFO;
#endif
    s_log_ready = 1;
}

void biz_log_set_level(unsigned char level)
{
    s_log_level = level;
}

unsigned char biz_log_get_level(void)
{
    return s_log_level;
}

const char *biz_log_level_name(unsigned char level)
{
    if (level <= BIZ_LOG_LEVEL_ERROR)
        return "error";
    if (level <= BIZ_LOG_LEVEL_WARN)
        return "warn";
    if (level <= BIZ_LOG_LEVEL_INFO)
        return "info";
    if (level <= BIZ_LOG_LEVEL_DEBUG)
        return "debug";
    return "print";
}

int biz_log_parse_level(const char *s)
{
    char *end = RT_NULL;
    unsigned long v;

    if (!s || !s[0])
        return -1;

    if (!rt_strcmp(s, "error") || !rt_strcmp(s, "err") || !rt_strcmp(s, "e"))
        return BIZ_LOG_LEVEL_ERROR;
    if (!rt_strcmp(s, "warn") || !rt_strcmp(s, "warning") || !rt_strcmp(s, "w"))
        return BIZ_LOG_LEVEL_WARN;
    if (!rt_strcmp(s, "info") || !rt_strcmp(s, "i"))
        return BIZ_LOG_LEVEL_INFO;
    if (!rt_strcmp(s, "debug") || !rt_strcmp(s, "dbg") || !rt_strcmp(s, "d"))
        return BIZ_LOG_LEVEL_DEBUG;
    if (!rt_strcmp(s, "print") || !rt_strcmp(s, "all"))
        return BIZ_LOG_LEVEL_PRINT;

    v = strtoul(s, &end, 0);
    if (end == s || (end && *end != '\0') || v > 255)
        return -1;
    return (int)v;
}

void biz_log_console_hook(const char *str)
{
    int len;

    if (!s_log_ready || s_in_biz_log_output || !str)
        return;

    len = (int)rt_strlen(str);
    if (len <= 0)
        return;

    rt_enter_critical();
    biz_log_ring_write(str, len, 0);
    rt_exit_critical();
}

#ifdef BSP_BIZ_LOG_TIMESTAMP
/* Boot-relative time via CNTPCT (us); for debug correlation only. */
static rt_uint64_t biz_log_now_us(void)
{
    rt_uint64_t cnt;
    rt_uint64_t freq;

    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0)
        return 0;
    return (cnt * 1000000ULL) / freq;
}
#endif

void biz_log_output(int level, const char *func, int line, const char *tag,
                    const char *fmt, ...)
{
    char log_buf[BIZ_LOG_ENTRY_MAX_LEN];
    int offset = 0;
    va_list args;

    /*
     * Compact console format:
     *   [I] message
     *   [D] func:line message   (location only at DEBUG)
     * Optional: BSP_BIZ_LOG_LOCATION  — always keep func:line
     * Optional: BSP_BIZ_LOG_TIMESTAMP — prefix [sec.us] boot-relative time
     */
#ifdef BSP_BIZ_LOG_TIMESTAMP
    {
        rt_uint64_t us = biz_log_now_us();
        offset = rt_snprintf(log_buf, sizeof(log_buf), "[%u.%06u] ",
                             (unsigned)(us / 1000000ULL),
                             (unsigned)(us % 1000000ULL));
        if (offset < 0 || (unsigned int)offset >= sizeof(log_buf))
            return;
    }
#endif

#if defined(BSP_BIZ_LOG_LOCATION)
    {
        int n = rt_snprintf(log_buf + offset, sizeof(log_buf) - (size_t)offset,
                            "[%s] %s:%d ",
                            tag ? tag : "?", func ? func : "?", line);
        if (n < 0 || (unsigned int)(offset + n) >= sizeof(log_buf))
            return;
        offset += n;
    }
#else
    if (level >= BIZ_LOG_LEVEL_DEBUG)
    {
        int n = rt_snprintf(log_buf + offset, sizeof(log_buf) - (size_t)offset,
                            "[%s] %s:%d ",
                            tag ? tag : "?", func ? func : "?", line);
        if (n < 0 || (unsigned int)(offset + n) >= sizeof(log_buf))
            return;
        offset += n;
    }
    else
    {
        int n = rt_snprintf(log_buf + offset, sizeof(log_buf) - (size_t)offset,
                            "[%s] ", tag ? tag : "?");
        if (n < 0 || (unsigned int)(offset + n) >= sizeof(log_buf))
            return;
        offset += n;
    }
#endif
    if (offset < 0 || (unsigned int)offset >= sizeof(log_buf))
        return;

    va_start(args, fmt);
    rt_vsnprintf(log_buf + offset, sizeof(log_buf) - (size_t)offset, fmt, args);
    va_end(args);

    /* Truncation can drop trailing '\n' → next line sticks on UART. */
    {
        size_t n = rt_strlen(log_buf);

        if (n + 1 < sizeof(log_buf) && (n == 0 || log_buf[n - 1] != '\n'))
        {
            log_buf[n] = '\n';
            log_buf[n + 1] = '\0';
        }
        else if (n + 1 >= sizeof(log_buf) && sizeof(log_buf) >= 2)
        {
            log_buf[sizeof(log_buf) - 2] = '\n';
            log_buf[sizeof(log_buf) - 1] = '\0';
        }
    }

    /*
     * Ring lives in .bss.noclean — may still hold a previous boot's header.
     * Never touch it until biz_log_init(); stray magic+bad offset was hanging
     * the first board LOG_I (after early mark E, before 'I').
     */
    if (s_log_ready)
    {
        rt_enter_critical();
        biz_log_ring_write(log_buf, (int)rt_strlen(log_buf),
                           level <= BIZ_LOG_LEVEL_WARN);
        rt_exit_critical();
    }

    s_in_biz_log_output = 1;
    rt_kprintf("%s", log_buf);
    s_in_biz_log_output = 0;
}
