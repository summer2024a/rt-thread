#!/usr/bin/env python3
"""
Apply EL3→EL2→EL1 transition fix based on bootwrapper reference.

Key changes from bootwrapper:
1. Before eret, set sctlr_el2 while in EL3
2. Use SPSR_KERNEL (0x3c5) masking for EL2 mode transition
3. Bootwrapper SCP timer configuration at 0x08600000
"""

import sys

FILE_PATH = "/work/rt-thread/libcpu/aarch64/cortex-a/entry_point.S"

def apply_patch():
    """Read, patch, and write the entry_point.S file."""
    
    # Read the file
    with open(FILE_PATH, 'r') as f:
        lines = f.readlines()
    
    # Find init_cpu_el function
    start_idx = None
    end_init_cpu_idx = None
    
    for i, line in enumerate(lines):
        if line.strip() == "init_cpu_el:":
            start_idx = i
            break
    
    if start_idx is None:
        print("ERROR: Could not find init_cpu_el: label")
        return False
    
    # Find the end of init_cpu_el (next function label)
    for i in range(start_idx + 1, len(lines)):
        stripped = lines[i].strip()
        if stripped and not stripped.startswith(';') and ':' in stripped and not stripped.startswith('.'):
            # Check if this is a new function (not a local label)
            if not stripped.startswith('.'):
                end_init_cpu_idx = i
                break
    
    if end_init_cpu_idx is None:
        end_init_cpu_idx = len(lines)
    
    print(f"Found init_cpu_el at line {start_idx + 1}")
    print(f"Function ends at line {end_init_cpu_idx + 1}")
    print(f"Function length: {end_init_cpu_idx - start_idx} lines")
    
    # Generate new init_cpu_el function based on bootwrapper
    new_init_cpu_el = """init_cpu_el:
    mrs     x0, CurrentEL           /* CurrentEL Register. bit 2, 3. Others reserved */
    lsr     x0, x0, #2
    and     x0, x0, #3

    /* Debug: print current EL as digit */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    add     w8, w0, #'0'
    strb    w8, [x9]

    /* Running at EL3? Switch to EL2 first (bootwrapper style) */
    cmp     x0, #3
    b.ne    .init_cpu_hyp_test

    /* ============ EL3 to EL2 transition (bootwrapper style) ============ */
    
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

    /* CRITICAL: Configure SCTLR_EL2 BEFORE eret (as bootwrapper does) */
    ldr     x1, =SCTLR_EL2_RESET    /* 0x30c51038: caches/MMU off, proper settings */
    msr     sctlr_el2, x1

    /* Print 'E' for EL3 setup complete */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'E'
    strb    w8, [x9]

    /* Set CPU frequency to 31250000 Hz (bootloader default) */
    mov     x9, #0x01dcd650
    msr     cntfrq_el0, x9

    /* Use SPSR_KERNEL (0x3c5) for EL2h transition - NOT manual bit ORing */
    mov     x1, #SPSR_KERNEL_EL2   /* SPSR_EL2H with all interrupts masked = 0x3c5 */
    get_phy x2, .init_cpu_hyp

    /* Switch to EL2 using eret */
    msr     elr_el3, x2
    msr     spsr_el3, x1
    eret

.init_cpu_hyp_test:
    /* Print '2' to indicate we're in EL2 mode */
    cmp     x0, #2
    b.lo    .print_el_marker
    b.hi    .init_cpu_sys            /* If > 2, skip to EL1 */

.print_el_marker:
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'2'
    strb    w8, [x9]

    /* ============ EL2 Setup (bootwrapper style) ============ */
    
.init_cpu_hyp:
    /* Print 'H' for EL2 hypervisor mode entered */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'H'
    strb    w8, [x9]
    
    /* Configure SCTLR_EL2 (bootwrapper style) */
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
    mov     w8, #0x01dcd650         /* 31250000 Hz */
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

    /* ============ EL2 to EL1 transition (bootwrapper style) ============ */
    
    /* Drop to EL1h using SPSR_KERNEL_EL1 */
    mov     x0, #SPSR_KERNEL_EL1   /* SPSR_EL1H = 0x385 */
    get_phy x2, .init_cpu_sys

    /* Switch to EL1 using eret */
    msr     elr_el2, x2
    msr     spsr_el2, x0
    eret

    /* ============ EL1 Setup (arm_system_registers config) ============ */
    
.init_cpu_sys:
    /* Print 'S' for EL1 system mode */
    ldr     x9, =HP232X_DEBUG_UART_BASE
    mov     w8, #'S'
    strb    w8, [x9]
    
    /* Configure SCTLR_EL1 */
    ldr     x0, =SCTLR_EL1_RESET    /* 0x30c50838 */
    msr     sctlr_el1, x0

    /* Enable EL0 virtual timer */
    mrs     x0, cntkctl_el1
    orr     x0, x0, #(1 << 1)
    msr     cntkctl_el1, x0

    /* Don't trap SIMD/FP instructions */
    mov     x0, #0x00300000
    msr     cpacr_el1, x0

    ret
"""

    # Replace the function
    new_lines = lines[:start_idx] + [new_init_cpu_el] + lines[end_init_cpu_idx:]
    
    # Write the file
    with open(FILE_PATH, 'w') as f:
        f.writelines(new_lines)
    
    print(f"Successfully patched {FILE_PATH}")
    print(f"Replaced {end_init_cpu_idx - start_idx} lines with {len(new_init_cpu_el.splitlines())} lines")
    return True

if __name__ == "__main__":
    if apply_patch():
        sys.exit(0)
    else:
        sys.exit(1)