/* HP232X EL3 to EL2/EL1 transition - Bootwrapper inspired */

.macro drop_el mode addr
    msr     elr_el3, \addr
    msr     spsr_el3, \mode
    eret
.endm

.text
.global test_el_transition
test_el_transition:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3

    /* Print current EL */
    ldr     x9, =0x10006000
    add     w8, w0, #'0'
    strb    w8, [x9]

    /* Running at EL3? */
    cmp     x0, #3
    bne     .already_at_el2

    /* EL3 to EL2 transition (bootwrapper style) */

    /* Configure Secure Configuration Register */
    mov     x0, #(1 << 0)           /* Non-secure EL0/EL1 */
    orr     x0, x0, #(1 << 4)       /* RES1 */
    orr     x0, x0, #(1 << 5)       /* RES1 */
    orr     x0, x0, #(1 << 8)       /* HVC enable */
    orr     x0, x0, #(1 << 10)      /* 64-bit EL2 */
    msr     scr_el3, x0

    /* Disable coprocessor traps to EL3 */
    msr     cptr_el3, xzr

    /* Configure SCTLR_EL2 BEFORE eret */
    ldr     x0, =0x08c51038         /* SCTLR_EL2_RESET: RES1 bits, caches/MMU off */
    msr     sctlr_el2, x0

    /* Print 'E' for EL3 complete */
    ldr     x9, =0x10006000
    mov     w8, #'E'
    strb    w8, [x9]

    /* Setup EL2h mode transition */
    mov     x1, #0x3c5              /* SPSR_KERNEL: EL2h with IRQ/FIQ/SError/Debug masked */
    ldr     x0, =.el2_entry
    drop_el x1, x0

.el2_entry:
    /* Print '2' for EL2 entry */
    ldr     x9, =0x10006000
    mov     w8, #'2'
    strb    w8, [x9]

    /* Linux kernel parameters match hệt the bootwrapper SCP configuration */
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
    mov     w8, #0x01dcd650         /* 31250000 Hz */
    str     w8, [x9]

    /* Configure counter frequency */
    mov     x9, #0x01dcd650
    msr     cntfrq_el0, x9

    /* Enable CNTP for EL1 */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #(1 << 0)       /* Don't trap EL0/1 phys counter */
    orr     x0, x0, #(1 << 1)       /* Don't trap EL0/1 phys timer */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    /* Print 'H' for hypervisor setup complete */
    ldr     x9, =0x10006000
    mov     w8, #'H'
    strb    w8, [x9]

 Already at EL2 (not from EL3) */
.already_at_el2:
    /* Print 'A' for already at EL2 */
    ldr     x9, =0x10006000
    mov     w8, #'A'
    strb    w8, [x9]

    /* Configure SCTLR_EL2 if not set */
    ldr     x0, =0x08c51038
    msr     sctlr_el2, x0

    /* Continue with EL2 setup */
    b       .el2_setup_complete

.el2_setup_complete:
    /* Setup EL1 transition */
    mov     x0, #0x3c5              /* SPSR_KERNEL_EL2 */
    ldr     x1, =.el1_entry

    /* EL2 to EL1 using eret */
    msr     elr_el2, x1
    msr     spsr_el2, x0
    eret

.el1_entry:
    /* Print '1' for EL1 entry */
    ldr     x9, =0x10006000
    mov     w8, #'1'
    strb    w8, [x9]

    /* Configure SCTLR_EL1 */
    ldr     x0, =0x30c50838
    msr     sctlr_el1, x0

    /* Enable EL0 virtual timer */
    mrs     x0, cntkctl_el1
    orr     x0, x0, #(1 << 1)
    msr     cntkctl_el1, x0

    /* Don't trap SIMD/FP instructions */
    mov     x0, #0x00300000
    msr     cpacr_el1, x0

    /* Print 'S' for system setup complete */
    ldr     x9, =0x10006000
    mov     w8, #'S'
    strb    w8, [x9]

    ret