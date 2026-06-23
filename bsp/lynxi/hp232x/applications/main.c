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
    rt_kprintf("CRIT: *** MAIN THREAD ENTRY ***\n");
    rt_kprintf("CRIT: Scheduler should be running now!\n");

    // Loop to confirm threads are being scheduled
    int counter = 0;
    for (;;) {
        volatile int i;
        for (i = 0; i < 500000; i++) { ; }
        rt_kprintf("CRIT: alive[%d]\n", ++counter);
    }

    return 0;
}
