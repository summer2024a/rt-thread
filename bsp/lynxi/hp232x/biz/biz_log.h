/*
 * biz_log.h — hp232x unified logging (ring + UART) with runtime level.
 *
 * Preferred API for drivers: HP_LOGE / HP_LOGW / HP_LOGI / HP_LOGD
 * Biz code may keep BIZ_* (same backend). LOG_* aliases replace rtdbg
 * when this header is included instead of <rtdbg.h>.
 *
 * msh: log | log info | log debug | log 4
 */

#ifndef _BIZ_LOG_H_
#define _BIZ_LOG_H_

#include <rtthread.h>
#include <stdint.h>
#include "board.h"

#define BIZ_LOG_BUFFER_SIZE    (8 * 1024)
#define BIZ_LOG_ENTRY_MAX_LEN  384
#define BIZ_LOG_MAGIC          0x53504C4Fu    /* "SPLO" */

#define BIZ_LOG_LEVEL_ERROR    3
#define BIZ_LOG_LEVEL_WARN     4
#define BIZ_LOG_LEVEL_INFO     6
#define BIZ_LOG_LEVEL_DEBUG    7
#define BIZ_LOG_LEVEL_PRINT    9

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t buffer_size;
    uint32_t write_offset;
    uint32_t wrap_count;
    uint32_t total_logs;
    uint32_t has_error;
    uint32_t is_frozen;
} biz_log_header_t;

typedef struct {
    biz_log_header_t header;
    char data[BIZ_LOG_BUFFER_SIZE - sizeof(biz_log_header_t)];
} biz_log_buffer_t;

extern biz_log_buffer_t g_log_buffer;
extern unsigned char s_log_level;

void biz_log_init(void);
void biz_log_set_level(unsigned char level);
unsigned char biz_log_get_level(void);
const char *biz_log_level_name(unsigned char level);
/* Parse "error"/"warn"/"info"/"debug"/"3"..; return -1 if invalid. */
int biz_log_parse_level(const char *s);
void biz_log_console_hook(const char *str);
void biz_log_output(int level, const char *func, int line, const char *tag,
                    const char *fmt, ...);

#define BIZ_LOG(lv, tag, fmt, args...) \
    do { \
        if ((lv) <= s_log_level) \
            biz_log_output(lv, __func__, __LINE__, tag, fmt, ##args); \
    } while (0)

#define BIZ_ERROR(args...)  BIZ_LOG(BIZ_LOG_LEVEL_ERROR, "E", ##args)
#define BIZ_WARN(args...)   BIZ_LOG(BIZ_LOG_LEVEL_WARN,  "W", ##args)
#define BIZ_INFO(args...)   BIZ_LOG(BIZ_LOG_LEVEL_INFO,  "I", ##args)
#define BIZ_DEBUG(args...)  BIZ_LOG(BIZ_LOG_LEVEL_DEBUG, "D", ##args)

/* Preferred names for drivers / board */
#define HP_LOGE(...) BIZ_ERROR(__VA_ARGS__)
#define HP_LOGW(...) BIZ_WARN(__VA_ARGS__)
#define HP_LOGI(...) BIZ_INFO(__VA_ARGS__)
#define HP_LOGD(...) BIZ_DEBUG(__VA_ARGS__)

/*
 * Compatibility with former rtdbg LOG_*: include this header and do NOT
 * include <rtdbg.h> for compile-time DBG_LVL filtering.
 */
#ifdef LOG_E
#undef LOG_E
#endif
#ifdef LOG_W
#undef LOG_W
#endif
#ifdef LOG_I
#undef LOG_I
#endif
#ifdef LOG_D
#undef LOG_D
#endif
#define LOG_E(...) HP_LOGE(__VA_ARGS__)
#define LOG_W(...) HP_LOGW(__VA_ARGS__)
#define LOG_I(...) HP_LOGI(__VA_ARGS__)
#define LOG_D(...) HP_LOGD(__VA_ARGS__)

#endif /* _BIZ_LOG_H_ */
