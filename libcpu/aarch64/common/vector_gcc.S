/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-10-06     ZhaoXiaowei  the first version
 * 2024-03-28     Shell        Move vector handling codes from context_gcc.S
 */

#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif

#include <rtconfig.h>

#ifdef BSP_USING_HP232X_SPIN_TABLE
    .extern hp232x_asm_dbg_trap
#else
    .extern hp232x_asm_dbg_putc
    .extern hp232x_asm_dbg_puts
    .extern hp232x_asm_dbg_hex64
#endif

.text
.globl system_vectors
.globl vector_exception
.globl vector_irq
.globl vector_fiq

system_vectors:
.align 11
    .set    VBAR, system_vectors
    .org    VBAR

    /*  Exception from CurrentEL (EL1) with SP_EL0 (SPSEL=1) */
    .org (VBAR + 0x00 + 0)
    b vector_serror                     /* Synchronous */
    .org (VBAR + 0x80 + 0)
    b vector_serror                     /* IRQ/vIRQ */
    .org (VBAR + 0x100 + 0)
    b vector_serror                     /* FIQ/vFIQ */
    .org (VBAR + 0x180 + 0)
    b vector_serror                     /* Error/vError */

    /*  Exception from CurrentEL (EL1) with SP_ELn */
    .org (VBAR + 0x200 + 0)
    b vector_exception                  /* Synchronous */
    .org (VBAR + 0x280 + 0)
    b vector_irq                        /* IRQ/vIRQ */
    .org (VBAR + 0x300 + 0)
    b vector_fiq                        /* FIQ/vFIQ */
    .org (VBAR + 0x380 + 0)
    b vector_serror

    /* Exception from lower EL, aarch64 */
    .org (VBAR + 0x400 + 0)
    b vector_exception
    .org (VBAR + 0x480 + 0)
    b vector_irq
    .org (VBAR + 0x500 + 0)
    b vector_fiq
    .org (VBAR + 0x580 + 0)
    b vector_serror

    /* Exception from lower EL, aarch32 */
    .org (VBAR + 0x600 + 0)
    b vector_serror
    .org (VBAR + 0x680 + 0)
    b vector_serror
    .org (VBAR + 0x700 + 0)
    b vector_serror
    .org (VBAR + 0x780 + 0)
    b vector_serror

#include "include/vector_gcc.h"
#define EFRAMEX x19

#ifndef BSP_USING_HP232X_SPIN_TABLE
    .macro hp232x_vec_puts label
    ldr     x9, =\label
    bl      hp232x_asm_dbg_puts
    .endm

    .macro hp232x_vec_puthex reg
    mov     x9, \reg
    bl      hp232x_asm_dbg_hex64
    .endm

    .macro hp232x_vec_dbg label
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    stp     x9, x10, [sp, #-16]!

    hp232x_vec_puts \label

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    hp232x_vec_puts .Lhp232x_vec_el
    hp232x_vec_puthex x0

    mrs     x1, esr_el1
    hp232x_vec_puts .Lhp232x_vec_esr
    hp232x_vec_puthex x1

    mrs     x2, elr_el1
    hp232x_vec_puts .Lhp232x_vec_elr
    hp232x_vec_puthex x2

    mrs     x3, far_el1
    hp232x_vec_puts .Lhp232x_vec_far
    hp232x_vec_puthex x3

    mov     x9, #'\n'
    bl      hp232x_asm_dbg_putc

    ldp     x9, x10, [sp], #16
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
    .endm
#endif

START_POINT(vector_exception)
#ifdef BSP_USING_HP232X_SPIN_TABLE
    /* Minimal exception handler - just spin forever */
1:
    wfe
    b       1b
#else
    /* Full exception handler with debug */
    SAVE_IRQ_CONTEXT
    mov     EFRAMEX, sp

    SAVE_USER_CTX EFRAMEX, x0

    mov     x0, EFRAMEX
    bl      rt_hw_trap_exception
    RESTORE_USER_CTX EFRAMEX, x0

    /* do exception switch for IRQ/exception handlers */
    EXCEPTION_SWITCH sp, x0

    RESTORE_IRQ_CONTEXT
    eret
#endif
START_POINT_END(vector_exception)

START_POINT(vector_serror)
    /* Minimal SError handler - just loop */
1:
    wfe
    b       1b
START_POINT_END(vector_serror)

START_POINT(vector_irq)
    /* Minimal IRQ handler - just loop */
1:
    wfe
    b       1b
START_POINT_END(vector_irq)

    .globl rt_hw_irq_exit
rt_hw_irq_exit:
    /* do exception switch for IRQ/exception handlers */
    EXCEPTION_SWITCH sp, x0

    RESTORE_IRQ_CONTEXT
    eret

