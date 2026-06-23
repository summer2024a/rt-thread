#!/bin/bash
# 手工修复EL3→EL2→EL1切换，遵循bootwrapper逻辑

FILE="/work/rt-thread/libcpu/aarch64/cortex-a/entry_point.S"

# 创建临时文件
TMPFILE=$(mktemp)

# 提取init_cpu_el函数开始之前的所有内容
awk '/^init_cpu_el:/ {exit} {print}' "$FILE" > "$TMPFILE"

# 添加新的init_cpu_el函数 (bootwrapper风格)
cat >> "$TMPFILE" << 'NEW_INIT_CPU_EL'

init_cpu_el:
    mrs     x0, CurrentEL           /* CurrentEL Register. bit 2, 3. Others reserved */
    lsr     x0, x0, #2
    and     x0, x0, #3

    /* Debug: print current EL */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    add     w8, w0, #'0'
    strb    w8, [x9]

    /* Running at EL3? Switch to EL2 first (bootwrapper style) */
    cmp     x0, #3
    b.ne    .init_cpu_hyp_test

    /* EL3 to EL2 transition - follow bootwrapper flow */

    /* Configure Secure Configuration Register */
    mov     x1, #(1 << 0)           /* EL0 and EL1 are in Non-Secure state */
    orr     x1, x1, #(1 << 4)       /* RES1 */
    orr     x1, x1, #(1 << 5)       /* RES1 */
    orr     x1, x1, #(1 << 8)       /* HVC enable */
    orr     x1, x1, #(1 << 10)      /* The next lower level is AArch64 */
    msr     scr_el3, x1
    msr     cptr_el3, xzr           /* Disable copro. traps to EL3 */

#ifdef BSP_USING_GICV3
    mov     x1, #((1 << 3) | (1 << 0))
    mrs     x2, S3_6_C12_C12_5
    orr     x2, x2, x1
    msr     S3_6_C12_C12_5, x2
    isb

    msr     S3_6_C12_C12_4, xzr
    isb
#endif

    /* Configure SCTLR_EL2 BEFORE eret (bootloader critical step) */
    ldr     x1, =SCTLR_EL2_RESET    /* Use ldr for big immediate */
    msr     sctlr_el2, x1

    /* Set CPU frequency to 31250000 Hz using movk for large value */
    mov     x9, #0x01dc
    movk    x9, #0xd650, lsl #16
    msr     cntfrq_el0, x9

    /* Print 'E' for EL3 setup complete */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'E'
    strb    w8, [x9]

    /* Prepare transition to EL2 (SPSR = 0x3c5 for EL2h with interrupts masked) */
    mov     x1, #0x3c5
    get_phy x2, .init_cpu_hyp

    /* Switch to EL2 using eret */
    msr     elr_el3, x2
    msr     spsr_el3, x1
    eret

.init_cpu_hyp_test:
    /* Debug: we're in EL2 mode */
    cmp     x0, #2
    b.lo    .print_el_marker
    b.hi    .init_cpu_sys            /* If > 2, skip to EL1 */

.print_el_marker:
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'2'
    strb    w8, [x9]

/* EL2 Setup (bootwrapper style) */
.init_cpu_hyp:
    /* Print 'H' for EL2 hypervisor mode */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'H'
    strb    w8, [x9]

    /* Configure SCTLR_EL2 */
    ldr     x1, =SCTLR_EL2_RESET
    msr     sctlr_el2, x1

    /* Bootwrapper SCP timer configuration */
    ldr     x9, =0x08600000         /* Timer CTRL base */
    mov     w8, #0x1
    str     w8, [x9]

    ldr     x9, =0x08600008         /* Timer route0 */
    mov     w8, #0xf
    str     w8, [x9]

    ldr     x9, =0x0860000c         /* Timer route1 */
    mov     w8, #0xf
    str     w8, [x9]

    ldr     x9, =0x08600020         /* Timer freq register */
    mov     w8, #0x01dc
    movk    w8, #0xd650, lsl #16
    str     w8, [x9]

    /* Enable CNTP for EL1 (allow EL0/1 access to physical counter/timer) */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #(1 << 0)       /* Don't trap EL0/1 physical counter */
    orr     x0, x0, #(1 << 1)       /* Don't trap EL0/1 physical timer */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr        /* Clear virtual offset */

    /* Print 'C' for CNTPCTL setup */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'C'
    strb    w8, [x9]

#ifdef BSP_USING_GICV3
    /* Enable GIC system register access */
    mrs     x0, S3_4_C12_C9_5
    orr     x0, x0, #(1 << 0)       /* Set ICC_SRE_EL2.SRE==1 */
    orr     x0, x0, #(1 << 3)       /* Set ICC_SRE_EL2.Enable==1 */
    msr     S3_4_C12_C9_5, x0
    isb
#endif

    /* Print 'G' for GIC setup complete */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'G'
    strb    w8, [x9]

    /* Drop to EL1h using SPSR = 0x385 (0x3c5 - 0x40 from EL2h to EL1h) */
    mov     x0, #0x385
    get_phy x2, .init_cpu_sys

    /* Switch to EL1 using eret */
    msr     elr_el2, x2
    msr     spsr_el2, x0
    eret

/* EL1 Setup */
.init_cpu_sys:
    /* Print 'S' for EL1 system mode */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'S'
    strb    w8, [x9]

    /* Configure SCTLR_EL1 */
    ldr     x0, =SCTLR_EL1_RESET
    msr     sctlr_el1, x0

    /* Enable EL0 virtual timer */
    mrs     x0, cntkctl_el1
    orr     x0, x0, #(1 << 1)
    msr     cntkctl_el1, x0

    /* Don't trap SIMD/FP instructions */
    mov     x0, #0x00300000
    msr     cpacr_el1, x0

    ret

NEW_INIT_CPU_EL

# 添加init_cpu_el之后的剩余内容 (init_kernel_bss及之后)
awk '/^init_kernel_bss:/,/^$/ {print; if (/^$/) exit}' "$FILE" >> "$TMPFILE"

# 验证文件
if [ -s "$TMPFILE" ]; then
    # 备份原文件
    cp "$FILE" "${FILE}.bak3"
    
    # 替换原文件
    mv "$TMPFILE" "$FILE"
    
    echo "成功修改 $FILE"
    echo "备份保存在 ${FILE}.bak3"
    
    # 显示新init_cpu_el函数的前50行
    echo ""
    echo "新 init_cpu_el 函数 (前50行):"
    awk '/^init_cpu_el:/,/^init_kernel_bss:/ {print; if (/^init_kernel_bss:/) exit}' "$FILE" | head -50
else
    echo "错误：临时文件为空"
    rm -f "$TMPFILE"
    exit 1
fi

cd /work/rt-thread/bsp/lynxi/hp232x && rm -rf build && scons -j4 2>&1 | tail -20