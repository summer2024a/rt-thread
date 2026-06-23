/* HP232X EL3→EL2→EL1 transition - standalone module #include "../common.h"

.text

.global hp232x_el_transition
hp232x_el_transition:
    /* Entry: x0 = CurrentEL value (already decrypted) */

    /* Debug: print current EL so we know where we came from */
    ldr     x9, =0x10006000         /* UART base */
    add     w8, w0, #'0'            /* Convert 0-3 to '0'-'3' */
    strb    w8, [x9]

    /* Branch based on current EL */
    cmp     x0, #3
    b.eq    .el3_entry
    cmp     x0, #2
    b.eq    .el2_entry
    /* x0 < 2 means EL1 or EL0, already in EL1 */
    b       .el1_setup

/* ====== Bootloader EL3→EL2 transition (matching bootwrapper) ====== */
.el3_entry:
    /* Configure Secure Configuration Register */
    mov     x1, #(1 << 0)           /* Non-secure EL0/EL1 */
    orr     x1, x1, #(1 << 4)       /* RES1 */
    orr     x1, x1, #(1 << 5)       /* RES1 */
    orr     x1, x1, #(1 << 8)       /* HVC enable */
    orr     x1, x1, #(1 << 10)      /* 64-bit EL2 */
    msr     scr_el3, x1

    /* Disable coprocessor traps to EL3 */
    msr     cptr_el3, xzr

    /* GIC init (if needed) */
#ifdef BSP_USING_GICV3
    mov     x1, #((1 << 3) | (1 << 0))
    mrs     x2, S3_6_C12_C12_5
    orr     x2, x2, x1
    msr     S3_6_C12_C12_5, x2
    isb

    msr     S3_6_C12_C12_4, xzr
    isb
#endif

    /* CRITICAL: Set SCTLR_EL2 BEFORE eret (bootwrapper established pattern) */
    ldr     x1, =0x08c51038         /* SCTLR_EL2: caches off, MMU off */
    msr     sctlr_el2, x1

    /* Print 'E' for EL3→EL2 complete */
    ldr     x9, =0x10006000
    mov     w8, #'E'
    strb    w8, [x9]

    /* Transition to EL2 (bootwrapper SPSR_KERNEL=0x3c5) */
    mov     x1, #0x3c5
    adr     x2, .el2_entry
    msr     elr_el3, x2
    msr     spsr_el3, x1
    eret

/* ====== Bootloader EL2→EL1 transition (matching bootwrapper) ====== */
.el2_entry:
    /* Print '2' for EL2 mode */
    ldr     x9, =0x10006000
    mov     w8, #'2'
    strb    w8, [x9]

    /* Reconfigure SCTLR_EL2 */
    ldr     x1, =0x08c51038
    msr     sctlr_el2, x1

    /* SCP timer configuration (from boot.S) */
    ldr     x9, =0x08600000         /* Timer CTRL base */
    mov     w8, #0x1
    str     w8, [x9]

    ldr     x9, =0x08600008         /* Timer route0 */
    mov     w8, #0xf
    str     w8, [x9]

    ldr     x9, =0x0860000c         /* Timer route1 */
    mov     w8, #0xf
    str     w8, [x9]

    ldr     x9, =0x08600020         /* Timer freq */
    mov     w8, #0x01dc
    movk    w8, #0xd650, lsl #16    /* 31250000 Hz */
    str     w8, [x9]

    /* Print 'H' for EL2 setup complete */
    ldr     x9, =0x10006000
    mov     w8, #'H'
    strb    w8, [x9]

    /* Enable CNTP for EL1 */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #(1 << 0)       /* Don't trap EL0/1 phys counter */
    orr     x0, x0, #(1 << 1)       /* Don't trap EL0/1 phys timer */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    /* Print 'C' for CNTP setup */
    ldr     x9, =0x10006000
    mov     w8, #'C'
    strb    w8, [x9]

#ifdef BSP_USING_GICV3
    /* Enable GIC system register interface */
    mrs     x0, S3_4_C12_C9_5
    orr     x0, x0, #(1 << 0)
    orr     x0, x0, #(1 << 3)
    msr     S3_4_C12_C9_5, x0
    isb
#endif

    /* Print 'G' for GIC setup */
    ldr     x9, =0x10006000
    mov     w8, #'G'
    strb    w8, [x9]

    /* Transition to EL1 (bootwrapper drops to EL1 using SPSR=0x385) */
    mov     x0, #0x385
    adr     x2, .el1_setup
    msr     elr_el2, x2
    msr     spsr_el2, x0
    eret

/* ====== EL1 final configuration ====== */
.el1_setup:
    /* Print 'S' for EL1 system mode */
    ldr     x9, =0x10006000
    mov     w8, #'S'
    strb    w8, [x9]

    /* Configure SCTLR_EL1 */
    ldr     x0, =0x30c50838
    msr     sctlr_el1, x0

    /* Enable EL0 virtual timer */
    mrs     x0, cntkctl_el1
    orr     x0, x0, #(1 << 1)
    msr     cntkctl_el1, x0

    /* Don't trap SIMD/FP */
    mov     x0, #0x00300000
    msr     cpacr_el1, x0

    ret