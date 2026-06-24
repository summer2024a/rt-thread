/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2025-06-17     lynxi          hp232x BSP — simplified main for IRAM-only system
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

int main(int argc, char** argv)
{
    rt_kprintf("Hi, this is RT-Thread!!\n");
    rt_thread_yield();
    return 0;
}
