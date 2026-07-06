/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif

#include "rtconfig.h"
#include "asm-generic.h"
#include "asm-fpu.h"
#include "armv8.h"

.section .text

START_POINT(_thread_start)
    mov     x0, x21
    blr     x19
    mov     x29, #0
    blr     x20
    b       .
START_POINT_END(_thread_start)
