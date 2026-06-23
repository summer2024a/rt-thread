#!/bin/bash
# Patch entry_point.S to fix EL3→EL2→EL1 transition based on bootwrapper reference

FILE="/work/rt-thread/libcpu/aarch64/cortex-a/entry_point.S"

echo "Current init_cpu_el function location:"
grep -n "^init_cpu_el:" "$FILE"

cat <<'EOF' > /tmp/el_transition_patch.patch
--- a/libcpu/aarch64/cortex-a/entry_point.S
+++ b/libcpu/aarch64/cortex-a/entry_point.S
@@ -373,17 +373,39 @@ init_cpu_el:
     mrs     x0, CurrentEL           /* CurrentEL Register. bit 2, 3. Others reserved */
     lsr     x0, x0, #2
     and     x0, x0, #3

-    /* running at EL3? */
+    /* Debug: print current EL */
+    ldr     x9, =HP232X_DEBUG_UART_BASE
+    add     w8, w0, #'0'
+    strb    w8, [x9]
+
+    /* Running at EL3? Switch to EL2 first */
     cmp     x0, #3
     b.ne    .init_cpu_hyp_test

-    /* should never be executed, just for completeness. (EL3) */
+    /* EL3 to EL2 transition (bootwrapper style) */
     mov     x1, #(1 << 0)           /* EL0 and EL1 are in Non-Secure state */
     orr     x1, x1, #(1 << 4)       /* RES1 */
     orr     x1, x1, #(1 << 5)       /* RES1 */
     orr     x1, x1, #(1 << 8)       /* HVC enable */
     orr     x1, x1, #(1 << 10)      /* The next lower level is AArch64 */
     msr     scr_el3, x1
     msr     cptr_el3, xzr           /* Disable copro. traps to EL3 */

 #ifdef BSP_USING_GICV3
     mov     x1, #((1 << 3) | (1 << 0))
@@ -395,12 +417,28 @@ init_cpu_el:
     msr     S3_6_C12_C12_4, xzr
     isb
 #endif

-    mov     x1, #9                  /* Next level is 0b1001->EL2h */
-    orr     x1, x1, #(1 << 6)       /* Mask FIQ */
-    orr     x1, x1, #(1 << 7)       /* Mask IRQ */
-    orr     x1, x1, #(1 << 8)       /* Mask SError */
-    orr     x1, x1, #(1 << 9)       /* Mask Debug Exception */
-    msr     spsr_el3, x1
+    /* Configure SCTLR_EL2 BEFORE eret (bootwrapper style) */
+    ldr     x1, =SCTLR_EL2_RESET
+    msr     sctlr_el2, x1
+
+    /* Print 'E' for EL3 setup complete */
+    ldr     x9, =HP232X_DEBUG_UART_BASE
+    mov     w8, #'E'
+    strb    w8, [x9]
+
+    /* Setup EL2h mode transition using SPSR_KERNEL_EL2 */
+    mov     x1, #SPSR_KERNEL_EL2   /* Bootwrapper SPSR_KERNEL = 0x3c5 */
+    get_phy x2, .init_cpu_hyp
+
+    /* Switch to EL2 using eret */
+    msr     elr_el3, x2
+    msr     spsr_el3, x1
+    eret

 .init_cpu_hyp_test:
+    /* Debug: we're in EL2 mode */
+    ldr     x9, =HP232X_DEBUG_UART_BASE
+    mov     w8, #'2'
+    strb    w8, [x9]
+
+    /* Running at EL2? Or already dropped to EL1? */
     cmp     x0, #2                  /* EL2 = 0b10  */
     b.ne    .init_cpu_sys

 .init_cpu_hyp:
-    ldr     x9, =0x10006000
+    /* Print 'H' for EL2 hypervisor mode */
+    ldr     x9, =HP232X_DEBUG_UART_BASE
     mov     w8, #'H'
     strb    w8, [x9]
-
-    ldr     x1, =0x30C50838
-    msr     sctlr_el2, x1
+
+    /* Configure SCTLR_EL2 (bootwrapper style) */
+    ldr     x1, =SCTLR_EL2_RESET
+    msr     sctlr_el2, x1
+
+    /* Bootwrapper SCP configuration for timer */
+    ldr     x9, =0x08600000         /* Timer CTRL base */
+    mov     w8, #0x1
+    str     w8, [x9]
+
+    ldr     x9, =0x08600008         /* Timer route0 */
+    mov     w8, #0xf
+    str     w8, [x9]
+
+    ldr     x9, =0x0860000c         /* Timer route1 */
+    mov     w8, #0xf
+    str     w8, [x9]
+
+    ldr     x9, =0x08600020         /* Timer freq */
+    mov     w8, #0x01dcd650         /* 31250000 Hz */
+    str     w8, [x9]
+
+    /* Set CPU frequency (31250000 Hz) */
+    mov     x9, #0x01dcd650
+    msr     cntfrq_el0, x9

     /* Enable CNTP for EL1 */
     mrs     x0, cnthctl_el2         /* Counter-timer Hypervisor Control register */
     orr     x0, x0, #(1 << 0)       /* Don't traps NS EL0/1 accesses to the physical counter */
     orr     x0, x0, #(1 << 1)       /* Don't traps NS EL0/1 accesses to the physical timer */
     msr     cnthctl_el2, x0
     msr     cntvoff_el2, xzr

-    ldr     x9, =0x10006000
+    /* Print 'C' for CNTPCTL setup */
+    ldr     x9, =HP232X_DEBUG_UART_BASE
     mov     w8, #'C'
     strb    w8, [x9]

 #ifdef BSP_USING_GICV3
-	mrs 	x0, S3_4_C12_C9_5
-	orr	    x0, x0, #(1 << 0)	    /* Set ICC_SRE_EL2.SRE==1 */
-	orr	    x0, x0, #(1 << 3)	    /* Set ICC_SRE_EL2.Enable==1 */
-	msr 	S3_4_C12_C9_5, x0
-	isb					        /*  Make sure SRE is now set */
+    mrs     x0, S3_4_C12_C9_5
+    orr     x0, x0, #(1 << 0)     /* Set ICC_SRE_EL2.SRE==1 */
+    orr     x0, x0, #(1 << 3)     /* Set ICC_SRE_EL2.Enable==1 */
+    msr     S3_4_C12_C9_5, x0
+    isb                         /* Make sure SRE is now set */
 #endif /* BSP_USING_GICV3 */

-    /* Removed problematic hcr_el2 write (causes hang) */
+    /* Print 'G' for GIC setup complete */
+    ldr     x9, =HP232X_DEBUG_UART_BASE
+    mov     w8, #'G'
+    strb    w8, [x9]
+
+    /* Setup EL1h mode transition using SPSR_KERNEL_EL1 */
+    mov     x0, #SPSR_KERNEL_EL1   /* Bootwrapper drops to EL1 using SPSR_EL1H = 0x3c5-0x40 = 0x385 */
+    get_phy x2, .init_cpu_sys
+
+    /* Switch to EL1 using eret */
+    msr     elr_el2, x2
+    msr     spsr_el2, x0
+    eret

+    ldr     x9, =HP232X_DEBUG_UART_BASE
+    mov     w8, #'L'
+    strb    w8, [x9]

-    mov     x0, #5                  /* Next level is 0b0101->EL1h */
-    orr     x0, x0, #(1 << 6)       /* Mask FIQ */
-    orr     x0, x0, #(1 << 7)       /* Mask IRQ */
-    orr     x0, x0, #(1 << 8)       /* Mask SError */
-    orr     x0, x0, #(1 << 9)       /* Mask Debug Exception */
-    msr     spsr_el2, x0
-
-    get_phy x0, .init_cpu_sys
-    msr     elr_el2, x0
-    eret
-
 .init_cpu_sys:
-    ldr     x9, =0x10006000
+    /* Print 'S' for EL1 system mode */
+    ldr     x9, =HP232X_DEBUG_UART_BASE
     mov     w8, #'S'
     strb    w8, [x9]
-
-    ldr     x0, =0x30C50838
+    
+    /* Configure SCTLR_EL1 */
+    ldr     x0, =SCTLR_EL1_RESET
     msr     sctlr_el1, x0

     mrs     x0, cntkctl_el1
     orr     x0, x0, #(1 << 1)      /* Set EL0VCTEN, enabling the EL0 Virtual Count Timer */
     msr     cntkctl_el1, x0

@@ -500,8 +538,6 @@ init_cpu_sys:
     /* Avoid trap from SIMD or float point instruction */
     mov     x0, #0x00300000         /* Don't trap any SIMD/FP instructions in both EL0 and EL1 */
     msr     cpacr_el1, x0

     ret
     dsb     ish
     isb
EOF

echo ""
echo "Patching $FILE ..."
cd /work/rt-thread
patch -p1 < /tmp/el_transition_patch.patch

echo ""
echo "Checking if patch was applied successfully:"
if [ $? -eq 0 ]; then
    echo "✓ Patch applied successfully!"
    echo ""
    echo "Verifying init_cpu_el function:"
    grep -A 100 "^init_cpu_el:" "$FILE" | head -50
else
    echo "✗ Patch failed. Trying manual fix..."
    exit 1
fi