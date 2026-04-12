/* applications/log.c */
#include <rtthread.h>

#ifdef RT_USING_ULOG
#include <ulog.h>
#include <dfs_posix.h>
#include <dfs_file.h>

#ifdef ULOG_BACKEND_USING_FILE
#include <ulog_be.h>
static struct ulog_file_be ulog_file_be;

static int ulog_file_init(void)
{
    struct stat st;


    if (mkdir("/log", 0777) < 0 && stat("/log", &st) < 0)
    {
        rt_kprintf("ulog: create /log failed, errno=%d\n", rt_get_errno());
        return -RT_ERROR;
    }

    /* 初始化文件后端 */
    ulog_file_backend_init(&ulog_file_be,
                           "kernel",           /* 后端名称 */
                           "/log",              /* 日志目录路径 */
                           5,                /* 最大文件数量 */
                           100 * 1024,       /* 最大文件大小 (100KB) */
                           256);            /* 缓冲区大小 */

    /* 启用文件后端 */
    ulog_file_backend_enable(&ulog_file_be);

    return RT_EOK;
}
INIT_APP_EXPORT(ulog_file_init);

#endif /* ULOG_BACKEND_USING_FILE */

#endif /* RT_USING_ULOG */