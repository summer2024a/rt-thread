/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2017-5-30     bernard       the first version
 */

#include <rtthread.h>

#ifdef BSP_USING_SDIO
#include <dfs_fs.h>

int mnt_init(void)
{
#ifdef BOARD_PKG_USING_LWEXT4
    /* delay 1s */
    rt_thread_delay(RT_TICK_PER_SECOND);
    if (dfs_mount("emmc7", "/", "ext", 0, 0) == 0)
    {
        rt_kprintf("file system initialization done!\n");
    } else {
        rt_kprintf("Fail file system mount, errno=%d\n", rt_get_errno());
    }
#else
    rt_thread_delay(RT_TICK_PER_SECOND/100);
    if (dfs_mount("sd1", "/", "ext", 0, 0) == 0)
    {
        rt_kprintf("file system initialization done!\n");
    }
    else if (dfs_mount("sd0", "/", "elm", 0, 0) == 0)
    {
        rt_kprintf("file system initialization done!\n");
    }
#endif

#ifdef RT_USING_DFS_ROMFS
    mkdir("/rom", 0777);
    extern const struct romfs_dirent romfs_root;
    if (dfs_mount(RT_NULL, "/rom", "rom", 0, &romfs_root) == 0)
    {
        rt_kprintf("ROM File System initialized!\n");
    }
#endif

    return 0;
}
INIT_ENV_EXPORT(mnt_init);
#endif
