#include <rtconfig.h>

#ifdef RT_USING_SMP

    .section .text.cpu_id, "ax"
    .align 2

/*
 * HP232X / KA200 — 2-core configuration
 *
 * MPIDR mapping:
 *   CPU0: MPIDR[8:0] = 0x000  -> CPU ID 0 (primary)
 *   CPU1: MPIDR[8:0] = 0x001  -> CPU ID 1 (secondary)
 */

.globl rt_hw_cpu_id_set
.type rt_hw_cpu_id_set, %function
rt_hw_cpu_id_set:
    mov x9, lr
    mrs x0, MPIDR_EL1
    and x1, x0, #15
    msr tpidr_el1, x1
    mov lr, x9
    RET

    .globl rt_hw_cpu_id
    .type rt_hw_cpu_id, %function

rt_hw_cpu_id:
    mrs     x0, mpidr_el1
    mov     x1, x0
    lsr     x2, x0, #8
    and     x2, x2, #0x1
    lsl     x2, x2, #2          /* cluster ID * 4 */
    and     x0, x1, #0xFF       /* core ID */
    cmp     x0, #4
    bge     .cpu_id_invalid
    add     x0, x2, x0
    ret

.cpu_id_invalid:
    mov     x0, #0
    ret

    .size rt_hw_cpu_id, .-rt_hw_cpu_id

    /* CPU MPIDR save function */
    .globl rt_hw_cpu_mpidr_save
    .type rt_hw_cpu_mpidr_save, %function

rt_hw_cpu_mpidr_save:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mrs     x1, mpidr_el1
    adrp    x2, rt_cpu_mpidr_table
    add     x2, x2, #:lo12:rt_cpu_mpidr_table
    lsl     x0, x0, #3
    add     x2, x2, x0
    str     x1, [x2]
    ldp     x29, x30, [sp], #16
    ret

    .size rt_hw_cpu_mpidr_save, .-rt_hw_cpu_mpidr_save

#endif /* RT_USING_SMP */

    .end
