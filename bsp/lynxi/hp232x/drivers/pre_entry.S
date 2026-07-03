/*
 * pre_entry.S - HP232X specific pre-entry initialization
 *
 * This file implements bootwrapper-style initialization for HP232X BSP,
 * including EL3 setup, GICv3 configuration, and EL transition.
 *
 * It is called by entry_point.S via `bl hp232x_bootwrapper_init`.
 *
 * Copyright (c) 2025 lynxi
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtconfig.h>

#ifdef BSP_USING_HP232X

#ifndef _HP232X_PRE_ENTRY_S_
#define _HP232X_PRE_ENTRY_S_

/* Required macros for bootwrapper-style initialization
 * These are standard ARMv8-A definitions used by bootwrapper
 */

/* MPIDR affinity bits mask (bootwrapper/boot.S) */
.equ MPIDR_ID_BITS, 0xff00ffffff

#ifdef BSP_USING_HP232X_DEBUG_UART
/*
 * UART debug interface - uses x6/x7 registers (same as entry_point.S)
 * Architecture:
 *   - x6: UART LSR address (pre-initialized)
 *   - x7: UART THR address (pre-initialized)
 *   - x8: Temporary register for character and LSR polling
 *
 * CRITICAL: x6/x7 must be initialized before using early_putc!
 */
.macro early_putc, char
// 1:  ldr     w8, [x6]              /* Read LSR (x6 pre-loaded) */
//     tst     w8, #0x20             /* Check THRE (Transmitter Holding Register Empty) */
//     b.eq    1b                    /* Wait if NOT ready */
//     mov     w8, \char             /* Load character */
//     str     w8, [x7]              /* Write to THR (x7 pre-loaded) */

ldr     x6, =0x10006000         /* UART0 base address (THR) */
1:  ldr     w8, [x6, #0x14]      /* 读 LSR = base + 0x14 */
    tst     w8, #0x20
    b.eq    1b
    mov     w8, \char
    str     w8, [x6]             /* 写 THR = base + 0 */
.endm
#else
.macro early_putc, char          /* Empty stub when DEBUG_UART disabled */
.endm
#endif /* BSP_USING_HP232X_DEBUG_UART */

    /* Keep current section (don't switch to .text) */

/* ===== HP232X Bootwrapper Entry Point ===== */
/* Following the WORKING version from 2026-06-23 that reached shell successfully */
/* CRITICAL: Match backup file - NO CurrentEL detection, direct EL3 init */

.global hp232x_bootwrapper_init
hp232x_bootwrapper_init:
    /* Check if primary CPU */
    mrs     x0, mpidr_el1
    ldr     x1, =0xff00ffffff      /* MPIDR_ID_BITS */
    tst     x0, x1
    b.ne    .hp232x_secondary_cpu_init  /* secondary cores jump to per-CPU init */

    /* ===== Primary CPU only: Full EL3 setup ===== */

    /* Set exception vector table FIRST */
    adr     x0, hp232x_exception_vectors_el3
    msr     vbar_el3, x0
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Initialize UART registers x6/x7 for early_putc macro */
    /* CRITICAL: early_putc depends on these registers! */
    ldr     x6, =0x10006014         /* UART0 LSR address */
    ldr     x7, =0x10006000         /* UART0 THR address */

    /* Use early_putc macro for all debug output */
    early_putc 'E'                  /* Print 'E' marker */

    /* Print CurrentEL */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    add     w8, w0, #'0'
    early_putc w8                   /* Print EL number */
#endif

    /* Step 1: Initialize Interconnect (CCI500) - backup file line 188-203 */
    ldr     x0, =0x9010000         /* CCI500 snoop control register base */
    mov     x1, #7                 /* Number of interfaces */

cci_init_loop:
    cmp     x1, #0
    b.eq    cci_init_done
    ldr     w3, [x0]
    lsr     w4, w3, #30
    orr     w3, w3, w4
    str     w3, [x0]
    mov     x5, #0x1000
    add     x0, x0, x5
    sub     x1, x1, #1
    b       cci_init_loop

cci_init_done:
    /* Step 2: Set CPUECTLR.SMPEn - backup file line 205-210 */
    mrs     x0, S3_1_c15_c2_1
    orr     x0, x0, #0x40
    msr     S3_1_c15_c2_1, x0
    isb

    /* Step 3: Set SCR_EL3 - backup file line 212-227 */
#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'S' marker before SCR_EL3 */
    early_putc 'S'
#endif

    mov     x0, #0x30
    orr     x0, x0, #(1 << 0)       /* NS=1 */
    orr     x0, x0, #(1 << 8)       /* HVC enable */
    orr     x0, x0, #(1 << 10)      /* RW=1 */
    msr     scr_el3, x0
    isb

    /* Re-initialize UART registers after SCR_EL3 */
#ifdef BSP_USING_HP232X_DEBUG_UART
    ldr     x6, =0x10006014
    ldr     x7, =0x10006000

    /* Print 'C' marker after SCR_EL3 */
    early_putc 'C'
#endif

    #ifdef BSP_USING_GICV3
    /* Step 2.1b: Enable GICv3 system register interface at EL3 */
    /* Reference: lynxi-bootwrapper/gic.S:196-200 */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'G' before GICv3 init */
    early_putc 'G'
#endif

    mov     x0, #((1 << 3) | (1 << 0))  /* Enable=1 | SRE=1 */
    mrs     x1, S3_6_C12_C12_5          /* ICC_SRE_EL3 */
    orr     x1, x1, x0
    msr     S3_6_C12_C12_5, x1         /* Enable ICC_SRE_EL3 */
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'I' after ICC_SRE_EL3 */
    early_putc 'I'
#endif

    /* Step 2.1c: Configure GICv3 Distributor at EL3 (critical for security) */
    /* Reference: lynxi-bootwrapper/gic.S:107-109 */
    ldr     x1, =0x08000000             /* GIC500 Distributor base */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* DEBUG: Print 'D' before GICD_CTLR config */
8:  ldr     w8, [x6]                /* Read LSR */
    tst     w8, #0x20               /* Check THRE */
    b.eq    8b
    mov     w8, #'D'                /* Print 'D' marker */
    str     w8, [x7]

    /* DEBUG: Read GICD_CTLR BEFORE write */
    ldr     w0, [x1]                /* Read GICD_CTLR old value */

    /* Print old value marker */
9:  ldr     w8, [x6]
    tst     w8, #0x20
    b.eq    9b
    mov     w8, #'X'                /* Print 'X' - before write */
    str     w8, [x7]
#endif

    /* Enable all groups and affinity routing */
    /* CRITICAL: Set DS bit (bit 6) to disable Security state separation */
    /* This allows Non-secure access to all GIC registers */
    mov     w0, #7                      /* EnableGrp0 | EnableGrp1ns | EnableGrp1s */
    orr     w0, w0, #(3 << 4)           /* ARE_S | ARE_NS (both bits 4 and 5) */
    orr     w0, w0, #(1 << 6)           /* DS=1 (Disable Security) */
    str     w0, [x1]                    /* GICD_CTLR = 0x77 */
    dsb     sy
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* DEBUG: Read GICD_CTLR AFTER write */
    ldr     w0, [x1]                /* Read GICD_CTLR new value */

    /* Print new value marker */
10: ldr     w8, [x6]
    tst     w8, #0x20
    b.eq    10b
    mov     w8, #'N'                /* Print 'N' - after write */
    str     w8, [x7]

    /* DEBUG: Print 'A' after GICD_CTLR config */
9:  ldr     w8, [x6]                /* Read LSR */
    tst     w8, #0x20               /* Check THRE */
    b.eq    9b
    mov     w8, #'A'                /* Print 'A' marker */
    str     w8, [x7]
#endif

    #ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'R' before loading Redistributor base */
    early_putc 'R'
#endif

    /* Step 2.1d: Configure Redistributor FIRST (before SPI) */
    /* Reference: lynxi-bootwrapper/gic.S:143-171 */
    ldr     x2, =0x08100000             /* GIC500 Redistributor base */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'B' after loading x2 base address */
    early_putc 'B'
#endif

    mvn     w5, wzr                     /* w5 = 0xffffffff */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'M' after mvn w5, wzr */
    early_putc 'M'
#endif

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'N' before Redistributor wakeup */
    early_putc 'N'
#endif

    /* Wakeup Redistributor: Clear ProcessorSleep, wait ChildrenAsleep */
    /* CRITICAL: Use w9 instead of w6 to avoid destroying x6 UART address! */
    mov     w9, #2                    /* First set w9 = 2 */
    mvn     w9, w9                    /* Then NOT: w9 = 0xFFFFFFFD (clear ProcessorSleep) */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print '2' after mvn */
    early_putc '2'
#endif

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print '3' before ldr w4, [x2, #0x014] */
    early_putc '3'
#endif

    ldr     w4, [x2, #0x014]            /* GICR_WAKER */
    and     w4, w4, w9                  /* Clear ProcessorSleep bit (use w9!) */
    str     w4, [x2, #0x014]            /* Write back to GICR_WAKER */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print '4' after ldr w4 */
    early_putc '4'
#endif

3:  ldr     w4, [x2, #0x014]            /* GICR_WAKER */
    ands    wzr, w4, #(1 << 2)          /* Test ChildrenAsleep bit */
    b.ne    3b                          /* Wait until cleared */

    /* Configure GICR after wakeup */
    add     x3, x2, #(1 << 16)          /* SGI_base offset (0x10000) */

    str     w5, [x3, #0x80]             /* GICR_IGROUP0 = 0xffffffff (all Group1) */
    str     wzr, [x3, #0xD00]           /* GICR_IGRPMOD0 = 0 (Non-secure) */

    /* ===== CRITICAL: Enable NS access to GICR registers ===== */
    /* Without this, Non-secure EL1 cannot modify group registers */
    /* GICR_NSACR: Allow Non-secure access to SGI/PPI configuration */
    mov     w11, #0xFFFFFFFF            /* Allow NS access to all SGI/PPI (0-31) */
    str     w11, [x3, #0xE00]           /* GICR_NSACR = 0xffffffff */
    dsb     sy
    isb

    /* ===== Enable SGI 0 for multi-core IPI communication ===== */
    /* Reference: u-boot SPL gic_64.S:107-108 */
    mov     w11, #0x1                   /* Enable SGI 0 */
    str     w11, [x3, #0x100]           /* GICR_ISENABLER0 */
    dsb     sy

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'I' after SGI enable */
    early_putc 'I'
#endif

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'N' after GICR config */
    early_putc 'N'
#endif

    /* Step 2.1e: Set SPI interrupts to Group1 (after Redistributor) */
    /* Reference: lynxi-bootwrapper/gic.S:173-183 */
    ldr     w2, [x1, #4]                /* GICD_TYPER */
    and     w2, w2, #0x1f               /* ITLinesNumber */
    cbz     w2, 2f                      /* Skip if no SPI interrupts */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'S' before SPI config */
    early_putc 'S'
#endif

    add     x3, x1, #0x84               /* GICD_IGROUP1 (SPI group register) */
    add     x4, x1, #0xD04              /* GICD_IGRPMOD1 (SPI mode register) */

1:  str     w5, [x3], #4                /* Set Group1 for 32 SPIs */
    str     wzr, [x4], #4               /* Set Non-secure mode */
    sub     w2, w2, #1
    cbnz    w2, 1b

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'P' after SPI config */
    early_putc 'P'
#endif

2:  /* GICv3 initialization complete */

    /* ===== Step 2.1f: ICC System Registers Configuration (lynxi-bootwrapper gic.S order) ===== */
    /* CRITICAL: Must set ICC_SRE_EL3 FIRST before any other ICC registers */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'E' before ICC enable setup */
    early_putc 'E'
#endif

    /* CRITICAL: Set ICC_SRE_EL3 FIRST (Enable | SRE) - like lynxi-bootwrapper gic.S:196-200 */
    mov     x10, #((1 << 3) | (1 << 0))      /* Enable | SRE */
    mrs     x11, S3_6_C12_C12_5              /* ICC_SRE_EL3 */
    orr     x11, x11, x10
    msr     S3_6_C12_C12_5, x11              /* ICC_SRE_EL3 */
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'S' after ICC_SRE_EL3 */
    early_putc 'S'
#endif

    /* Configure ICC_CTLR_EL3 for Non-Secure access */
    msr     S3_6_C12_C12_4, xzr              /* ICC_CTLR_EL3 = 0 */
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'C' after ICC_CTLR_EL3 */
    early_putc 'C'
#endif

    /* Now safe to set ICC_IGRPEN1_EL3 (after SRE is enabled) */
    mov     x10, #0x3                          /* EnableGrp1NS=1 | EnableGrp1S=1 */
    msr     S3_6_C12_C12_7, x10              /* ICC_IGRPEN1_EL3 */
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print '1' after ICC_IGRPEN1_EL3 */
    early_putc '1'
#endif

    /* ICC_SRE_EL2: Allow EL1 access to ICC system registers */
    mrs     x10, S3_4_C12_C9_5      /* Read ICC_SRE_EL2 */
    orr     x10, x10, #0xf          /* SRE=1 | Disable IRQ/FIQ bypass | Allow EL1 access */
    msr     S3_4_C12_C9_5, x10      /* ICC_SRE_EL2 */
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print '2' after ICC_SRE_EL2 */
    early_putc '2'
#endif

    /* ICC_PMR_EL1: Allow Non-secure access to priority mask (CRITICAL!) */
    mov     x10, #0x80              /* Non-Secure access to ICC_PMR_EL1 (bit 7) */
    msr     S3_0_C4_C6_0, x10       /* ICC_PMR_EL1 */
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'M' after ICC_PMR_EL1 */
    early_putc 'M'
#endif

    /* ICC_CTLR_EL1: Non-secure CPU interface configuration */
    msr     S3_0_C12_C12_4, xzr      /* ICC_CTLR_EL1 = 0 */
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'C' after ICC_CTLR_EL1 */
    early_putc 'C'
#endif

#endif /* BSP_USING_GICV3 */

    /* Step 5: Disable coprocessor traps to EL3 (boot.S:55) */
    msr     cptr_el3, xzr

    /* Step 6: Set timer frequency (boot.S:75-77) */
    ldr     x0, =31250000           /* cnt_freq value */
    msr     cntfrq_el0, x0

    /* Step 7: Set SCTLR_EL2 (spin.S:22-23) */
    ldr     x0, =(3 << 28 | 3 << 22 | 1 << 18 | 1 << 16 | 1 << 11 | 3 << 4)  /* SCTLR_EL2_RESET */
    msr     sctlr_el2, x0

    /* Step 8: Set SPSR_EL3 and ELR_EL3 for eret (spin.S:24-26) */
    adr     x0, .hp232x_after_eret   /* Return address after eret */
    mov     x1, #(9 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))  /* SPSR_KERNEL: EL2h + all exceptions masked */
    msr     elr_el3, x0
    msr     spsr_el3, x1

    /* Step 9: Execute eret */
    eret

.hp232x_after_eret:
    /* At EL2 now: Configure HCR_EL2 to route IRQ/FIQ/SError to EL1 */
    /* CRITICAL: Without this, IRQ will be routed to EL2 and EL1 cannot handle interrupts! */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Re-initialize UART registers after eret (context may be lost) */
    ldr     x6, =0x10006014         /* UART0 LSR */
    ldr     x7, =0x10006000         /* UART0 THR */

    /* NOTE: UART may not be accessible from EL2 — skip early_putc markers here */
    /* early_putc 'H' */
#endif

    /* Set HCR_EL2: Route all interrupts to EL1 */
    mov     x0, #(1 << 31)          /* RW=1: 64-bit EL1 */
    /* CRITICAL: Clear interrupt routing bits to route IRQ/FIQ/SError to EL1 */
    bic     x0, x0, #(1 << 4)       /* IMO=0: IRQ route to EL1 (not EL2) */
    bic     x0, x0, #(1 << 5)       /* FMO=0: FIQ route to EL1 (not EL2) */
    bic     x0, x0, #(1 << 6)       /* AMO=0: SError route to EL1 (not EL2) */
    bic     x0, x0, #(1 << 24)      /* TTC=0: Allow EL1 to write CNTP_CTL_EL0 */
    bic     x0, x0, #(1 << 25)      /* TSC=0: Allow EL1 to access timer sysregs */
    bic     x0, x0, #(1 << 26)      /* TWE=0: Allow EL1 WFI */
    bic     x0, x0, #(1 << 27)      /* TWI=0: Allow EL1 WFE */
    bic     x0, x0, #(1 << 28)      /* TPC=0: Allow EL1 to access physical counter */
    bic     x0, x0, #(1 << 29)      /* TPIDRURO=0: Allow EL1 access to TPIDRURO */
    bic     x0, x0, #(1 << 30)      /* TPIDRURW=0: Allow EL1 access to TPIDRURW */
    /* TRVM (bit 31) must NOT be cleared — it conflicts with RW (bit 31) */
    msr     hcr_el2, x0
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* NOTE: UART may not be accessible from EL2 */
    /* early_putc 'L' */
#endif

    /* Set SCTLR_EL1 */
    mov     x0, #0
    msr     sctlr_el1, x0
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'O' marker - SCTLR_EL1 set */
    early_putc 'O'
#endif

    /* Set SPSR_EL2 and ELR_EL2 for EL2→EL1 */
    adr     x0, .hp232x_el1_continue  /* Return to EL1 continuation */
    mov     x1, #(5 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))  /* SPSR: EL1h + all exceptions masked */
    msr     elr_el2, x0
    msr     spsr_el2, x1
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Print 'W' marker - Ready for EL2→EL1 eret */
    early_putc 'W'
#endif

    /* EL2→EL1 transition */
    eret

/* ===== Secondary CPU Initialization (Per-CPU) ===== */

.hp232x_secondary_cpu_init:
    /* Secondary CPU in EL3: perform per-CPU GIC init and EL transition */
    /* CRITICAL: Set exception vector table FIRST */
    adr     x0, hp232x_exception_vectors_el3
    msr     vbar_el3, x0
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Initialize UART debug registers for secondary CPUs */
    ldr     x6, =0x10006014         /* UART0 LSR */
    ldr     x7, =0x10006000         /* UART0 THR */

    /* DEBUG: Print 'W' marker for secondary CPU - DISABLED to avoid race */
//    ldr     w8, [x6]
//    tst     w8, #0x20
//    b.eq    sec_debug_w
//    mov     w8, #'W'                /* Print 'W' - Secondary CPU wake */
//    str     w8, [x7]
//sec_debug_w:
#endif

    /* Step 1: Per-CPU GIC Redistributor Configuration */
    /* Calculate current CPU's Redistributor base address */

    /* Get current CPU affinity */
    mrs     x10, mpidr_el1
    and     x11, x10, #0xFF            /* Aff0 [7:0] */
    ubfx    x12, x10, #8, #8           /* Aff1 [15:8] */

    /* Calculate linear CPU ID: LinearID = Aff1 * 4 + Aff0 */
    lsl     x12, x12, #2               /* Aff1 * 4 */
    add     x11, x11, x12              /* Linear ID */
    sub     x11, x11, #1               /* Secondary CPUs: index starts from 1 */

    /* Calculate Redistributor base for this CPU */
    ldr     x2, =0x08100000            /* Redistributor base */
    mov     x13, #(2 << 16)            /* Each Redistributor = 128KB */
    mul     x14, x11, x13              /* Offset = LinearID * 128KB */
    add     x2, x2, x14                /* Current CPU Redistributor base */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* DEBUG markers DISABLED for secondary CPUs to avoid UART race */
//    ldr     w8, [x6]
//    tst     w8, #0x20
//    b.eq    sec_debug_r
//    mov     w8, #'R'
//    str     w8, [x7]
//sec_debug_r:
#endif

    /* Wakeup Redistributor: Clear ProcessorSleep */
    mov     w9, #2
    mvn     w9, w9                     /* w9 = 0xFFFFFFFD */
    ldr     w4, [x2, #0x014]           /* GICR_WAKER */
    and     w4, w4, w9                 /* Clear ProcessorSleep bit */
    str     w4, [x2, #0x014]
    dsb     sy
    isb

    /* Wait for ChildrenAsleep to clear */
sec_wait_waker:
    ldr     w4, [x2, #0x014]
    ands    wzr, w4, #(1 << 2)
    b.ne    sec_wait_waker

    /* Configure SGI/PPI interrupt groups */
    add     x3, x2, #(1 << 16)         /* SGI_base */
    mvn     w5, wzr                    /* w5 = 0xFFFFFFFF */
    str     w5, [x3, #0x80]            /* GICR_IGROUP0 = 0xFFFFFFFF */
    str     wzr, [x3, #0xD00]          /* GICR_IGRPMOD0 = 0x00000000 */

    /* ===== CRITICAL: Enable NS access to GICR registers ===== */
    mov     w11, #0xFFFFFFFF           /* Allow NS access to all SGI/PPI */
    str     w11, [x3, #0xE00]          /* GICR_NSACR = 0xffffffff */
    dsb     sy
    isb

    /* Enable SGI 0 for IPI */
    mov     w11, #0x1
    str     w11, [x3, #0x100]          /* GICR_ISENABLER0 */
    dsb     sy

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* DEBUG markers DISABLED for secondary CPUs */
//    ldr     w8, [x6]
//    tst     w8, #0x20
//    b.eq    sec_debug_g
//    mov     w8, #'G'
//    str     w8, [x7]
//sec_debug_g:
#endif

#ifdef BSP_USING_GICV3
    /* Step 2: ICC System Registers Configuration */
    /* Same as primary CPU: enable Group1, allow EL1 access */

    /* ICC_IGRPEN1_EL3: Enable Group 1 interrupts */
    mov     x10, #0x3
    msr     S3_6_C12_C12_7, x10         /* ICC_IGRPEN1_EL3 */
    isb

    /* ICC_SRE_EL2: Allow EL1 access */
    mrs     x10, S3_4_C12_C9_5
    orr     x10, x10, #0xf
    msr     S3_4_C12_C9_5, x10
    isb

    /* ICC_PMR_EL1: Allow NS priority access */
    mov     x10, #0x80
    msr     S3_0_C4_C6_0, x10           /* ICC_PMR_EL1 */
    isb

    /* ICC_CTLR_EL1: Non-secure CPU interface */
    msr     S3_0_C12_C12_4, xzr
    isb

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* DEBUG markers DISABLED for secondary CPUs */
//    ldr     w8, [x6]
//    tst     w8, #0x20
//    b.eq    sec_debug_c
//    mov     w8, #'C'
//    str     w8, [x7]
//sec_debug_c:
#endif
#endif

    /* Step 3: EL Transition (EL3 → EL2 → EL1) */
    /* Same as primary CPU: need to transition to EL1 before spin wait */

    /* Set SCR_EL3 for EL transition */
    mov     x0, #0x30               /* RES1 bits */
    orr     x0, x0, #(1 << 0)       /* NS=1 */
    orr     x0, x0, #(1 << 8)       /* HVC enable */
    orr     x0, x0, #(1 << 10)      /* RW=1: 64-bit EL2 */
    msr     scr_el3, x0
    isb

    /* Set SCTLR_EL2 */
    ldr     x0, =(3 << 28 | 3 << 22 | 1 << 18 | 1 << 16 | 1 << 11 | 3 << 4)
    msr     sctlr_el2, x0
    isb

    /* Set SPSR_EL3 and ELR_EL3 for EL3→EL2 */
    adr     x0, .hp232x_secondary_el2
    mov     x1, #(9 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))
    msr     elr_el3, x0
    msr     spsr_el3, x1
    isb

    /* EL3→EL2 transition */
    eret

.hp232x_secondary_el2:
    /* At EL2: Continue EL2→EL1 transition */

    /* Set HCR_EL2: Route all interrupts to EL1 */
    mov     x0, #(1 << 31)          /* RW=1: 64-bit EL1 */
    /* CRITICAL: Clear interrupt routing bits to route IRQ/FIQ/SError to EL1 */
    bic     x0, x0, #(1 << 4)       /* IMO=0: IRQ route to EL1 (not EL2) */
    bic     x0, x0, #(1 << 5)       /* FMO=0: FIQ route to EL1 (not EL2) */
    bic     x0, x0, #(1 << 6)       /* AMO=0: SError route to EL1 (not EL2) */
    msr     hcr_el2, x0
    isb

    /* Set SCTLR_EL1 */
    mov     x0, #0
    msr     sctlr_el1, x0
    isb

    /* Set SPSR_EL2 and ELR_EL2 for EL2→EL1 */
    adr     x0, .hp232x_secondary_el1
    mov     x1, #(5 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))
    msr     elr_el2, x0
    msr     spsr_el2, x1
    isb

    /* EL2→EL1 transition */
    eret

.hp232x_secondary_el1:
    /* At EL1: Enter spin wait loop */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* DEBUG markers DISABLED for secondary CPUs */
//    ldr     w8, [x6]
//    tst     w8, #0x20
//    b.eq    sec_debug_s
//    mov     w8, #'S'
//    str     w8, [x7]
//sec_debug_s:
#endif

    /* Jump to secondary spin wait */
    b       .hp232x_secondary_spin

/* ===== EL1 Entry Points ===== */

.hp232x_el1_boot:
    /* Already at EL1: Skip EL3 initialization, direct boot */
    b       .hp232x_el1_continue

.hp232x_el1_continue:
    /* At EL1: Enable MMU before returning to entry_point.S */
    /* Set TTBR0_EL1 */
    ldr     x0, =hp232x_static_pgd
    msr     ttbr0_el1, x0
    isb

    /* Set MAIR_EL1 */
    ldr     x0, =0x00447f
    msr     mair_el1, x0
    isb

    /* Set TCR_EL1 */
    ldr     x0, =0x0000000200000000
    orr     x0, x0, #(3 << 12)
    orr     x0, x0, #(1 << 10)
    orr     x0, x0, #(1 << 8)
    msr     tcr_el1, x0
    isb

    /* Enable MMU and caches in SCTLR_EL1 */
    mrs     x0, sctlr_el1
    mov     x1, #0x1
    movk    x1, #0x5001, lsl #16
    orr     x0, x0, x1
    msr     sctlr_el1, x0
    isb

    /* CRITICAL: Clear SPSR_EL1 to unmask all exceptions on return */
    /* Otherwise 'ret' restores D+A+I+F=1 from SPSR_EL1, blocking all interrupts */
    mov     x0, #0
    msr     spsr_el1, x0
    isb

    /* Return to entry_point.S */
    ret

/* ===== Static Page Table Data (hardcoded, stored in IRAM0 .text) ===== */

.align 12
hp232x_static_pgd:
    /* PGD[0]: Points to PUD table (covers VA 0-512GB) */
    .quad hp232x_static_pud + 0x3    /* Table descriptor: addr | type=3 */

.align 12
hp232x_static_pud:
    /* PUD[0]: Points to PMD table for 0GB-1GB region */
    .quad hp232x_static_pmd + 0x3    /* Table descriptor */
    .quad 0
    .quad 0
    .quad 0
    /* PUD[4]: IRAM1 - maps VA 0x100050000-0x13FFFFFFF to PA 0x100000000 */
    /* IRAM1 physical base 0x100000000, VA base 0x100050000 */
    /* Need VA-aligned mapping: VA 0x100050000 → PA 0x100050000 */
    /* But IRAM1 PA is 0x100000000, so we map VA 0x100000000 to PA 0x100000000 */
    /* and rely on board.c MMU setup for the VA offset correction */
    .quad 0x100000601 // VA 0x100000000 -> PA 0x100000000

.align 12
hp232x_static_pmd:
    /* PMD covers VA 0GB-1GB, each entry = 2MB block */
    .rept 32
    .quad 0
    .endr
    /* PMD[32-35]: IRAM0 (64MB-72MB area, Normal Memory) */
    .quad 0x04000601
    .quad 0x04200601
    .quad 0x04400601
    .quad 0x04600601
    /* PMD[36-63]: Unused (72MB-128MB) */
    .rept 28
    .quad 0
    .endr
    /* PMD[64-71]: GIC Distributor (128MB area, Device Memory) */
    .quad 0x08000602
    .quad 0x08200602
    .quad 0x08400602
    .quad 0x08600602
    .quad 0x08800602
    .quad 0x08A00602
    .quad 0x08C00602
    .quad 0x08E00602
    /* PMD[72-127]: Unused (144MB-256MB) */
    .rept 56
    .quad 0
    .endr
    /* PMD[128]: UART (256MB area, Device Memory) */
    .quad 0x10000602
    /* PMD[129-511]: Unused (rest of 1GB) */
    .rept 383
    .quad 0
    .endr

/* ===== Secondary CPU Spin Wait ===== */

.hp232x_secondary_spin:
    /* Secondary CPU: spin wait for mailbox release (bootwrapper spin.S:46-59) */
    /* Calculate per-CPU mbox entry */
    mrs     x5, mpidr_el1
    and     x6, x5, #0xFF            /* Aff0 [7:0] */
    ubfx    x7, x5, #8, #8           /* Aff1 [15:8] */
    lsl     x7, x7, #2               /* Aff1 * 4 (cluster size) */
    add     x5, x6, x7               /* Linear ID = Aff1*4 + Aff0 */
    sub     x5, x5, #1               /* Secondary CPUs index 0-6 */
    lsl     x5, x5, #3               /* * 8 bytes per entry */

    /* Spin table base — use linker symbol, not hardcoded 0x0401FF00 */
    ldr     x4, =hp232x_spin_table
    add     x4, x4, x5              /* Current CPU's mbox entry */

    /* CRITICAL: Clear mailbox to 0 to reject garbage from bootcode
     * (e.g. 0x2323232323232323 left in .bss). Ensures secondary CPU
     * only wakes up when primary CPU explicitly releases it. */
    str     xzr, [x4]
    dsb     sy
    isb

spin_wait_loop:
    wfe
    ldr     x1, [x4]                /* Read entry point */
    cbz     x1, spin_wait_loop      /* Zero means not released */
    /* Double-check: entry must be in IRAM0 (0x04000000-0x040FFFFF) */
    /* Reject any residual garbage despite the clear above */
    movz    x2, #0x0400
    movk    x2, #0x0000, lsl #16
    cmp     x1, x2
    blt     spin_wait_loop
    movz    x2, #0x0410
    cmp     x1, x2
    bge     spin_wait_loop
    br      x1                       /* Safe to jump */

/* ===== HP232X Data Sections ===== */

    .section .early_hp232x, "aw", @progbits

/* HP232X secondary CPU spin table - placed in IRAM0 first 256KB (constraint-compliant) */
    .align 12
hp232x_spin_table:
    .space 64                   /* Spin table for up to 8 CPUs */
hp232x_spin_table_end:

/* HP232X stack area - placed in IRAM1 last 256KB (constraint-compliant) */
    .align 4
hp232x_stack_bottom:
    .space 256                  /* 256 bytes per CPU (bootwrapper STACK_SIZE) */
hp232x_stack_top:

/* ===== HP232X Exception Vector Table Section (executable) ===== */
    .section .hp232x_vectors, "ax", @progbits
    .align 11                   /* Exception vectors must be 2KB-aligned (0x800) */

.global hp232x_exception_vectors_el3
hp232x_exception_vectors_el3:

    /* Current EL with SP0 (0x000-0x7FF) */
    .rept 128
    b hp232x_exception_handler_el3
    .endr

    /* Current EL with SPx (0x800-0xFFF) */
    .rept 128
    b hp232x_exception_handler_el3
    .endr

    /* Lower EL using AArch64 (0x400-0x7FF) */
    .rept 128
    b hp232x_exception_handler_el3
    .endr

    /* Lower EL using AArch32 (0xC00-0xFFF) */
    .rept 128
    b hp232x_exception_handler_el3
    .endr

hp232x_exception_handler_el3:
    /* Ultra-simplified exception handler: minimal UART output */
    /* Only output one marker character, no address printing */

#ifdef BSP_USING_HP232X_DEBUG_UART
    /* Hardcoded UART THR address - simplest possible */
    /* UART THR offset is 0x10006000, but we use relative addressing */
    adr     x6, exc_simple_uart      /* Load nearby UART THR address */
    ldr     x6, [x6]                 /* Read the address */

    /* Just output '?' to indicate exception */
    mov     w8, #'?'
    str     w8, [x6]                 /* Direct write, no wait */

    /* Infinite halt loop */
exc_halt:
    wfe
    b       exc_halt

exc_simple_uart:
    .quad   0x10006000               /* UART THR address */

#else
    /* No UART debug: just halt */
exc_halt:
    wfe
    b       exc_halt
#endif

#endif /* _HP232X_PRE_ENTRY_S_ */

/* HP232X temporary stack in IRAM0 (last 256KB, constraint-compliant) */
    .align 12
    .global hp232x_temp_stack_top
hp232x_temp_stack:
    .space 32768              /* 32KB temp stack in IRAM0 */
hp232x_temp_stack_top:

#endif /* BSP_USING_HP232X */

/*
 * ===== 方案A: Assembly硬编码静态页表（备用方案） =====
 * 方案B（C代码）当前被调用，此方案作为备用
 * 完整的静态页表定义，只在启用BSP_USING_HP232X_USE_STATIC_ASM时编译
 */
#ifdef BSP_USING_HP232X_USE_STATIC_ASM

/* ===== Static Page Table Data (hardcoded, stored in IRAM0 .text) ===== */

.align 12
hp232x_static_pgd:/*
 * ===== 方案A: Assembly硬编码静态页表（保留但未使用） =====
 * 方案B（C代码）当前被调用，此方案作为备用
 */
#ifdef BSP_USING_HP232X_USE_STATIC_ASM  /* 暂不启用 */

/* ===== Static Page Table Data (hardcoded, stored in IRAM0 .text) ===== */

.align 12
hp232x_static_pgd:
    /* PGD[0]: Points to PUD table (covers VA 0-512GB) */
    .quad hp232x_static_pud + 0x3    /* Table descriptor: addr | type=3 */

.align 12
hp232x_static_pud:
    /* PUD[0]: Points to PMD table for 0GB-1GB region (IRAM0 + devices) */
    .quad hp232x_static_pmd + 0x3    /* Table descriptor */

    /* PUD[1-3]: Unused (save space) */
    .quad 0
    .quad 0
    .quad 0

    /* PUD[4]: IRAM1 @ 4GB (1GB block, Normal Memory) */
    .quad 0x100000601 // VA 0x100000000 -> PA 0x100000000                 /* (4 << 30) | 0x600 | 0x1 */

.align 12
hp232x_static_pmd:
    /* PMD covers VA 0GB-1GB, each entry = 2MB block */
    /* Descriptor format: (2MB_index << 21) | attrs | type */

    /* PMD[0-31]: Unused (0-64MB) */
    .rept 32
    .quad 0
    .endr

    /* PMD[32-35]: IRAM0 (64MB-72MB area, Normal Memory) */
    .quad 0x04000601                  /* PMD[32]: (32 << 21) | 0x600 | 0x1 */
    .quad 0x04200601                  /* PMD[33]: IRAM0 continued */
    .quad 0x04400601                  /* PMD[34] */
    .quad 0x04600601                  /* PMD[35] */

    /* PMD[36-63]: Unused (72MB-128MB) */
    .rept 28
    .quad 0
    .endr

    /* PMD[64-71]: GIC Distributor (128MB area, Device Memory) */
    .quad 0x08000602                  /* PMD[64]: (64 << 21) | 0x602 | 0x1 */
    .quad 0x08200602                  /* PMD[65] */
    .quad 0x08400602                  /* PMD[66] */
    .quad 0x08600602                  /* PMD[67] */
    .quad 0x08800602                  /* PMD[68] */
    .quad 0x08A00602                  /* PMD[69] */
    .quad 0x08C00602                  /* PMD[70] */
    .quad 0x08E00602                  /* PMD[71] */

    /* PMD[72-127]: Unused (144MB-256MB) */
    .rept 56
    .quad 0
    .endr

    /* PMD[128]: UART (256MB area, Device Memory) */
    .quad 0x10000602                  /* PMD[128]: (128 << 21) | 0x602 | 0x1 */

    /* PMD[129-511]: Unused (rest of 1GB) */
    .rept 383
    .quad 0
    .endr

hp232x_enable_mmu_static:
    /* 方案A实现（硬编码静态页表） */
    /* Save return address */
    mov     x30_saved, x30

    /* Set TTBR0_EL1 */
    ldr     x0, =hp232x_static_pgd
    msr     ttbr0_el1, x0
    isb

    /* Set MAIR_EL1 */
    ldr     x0, =0x00447f
    msr     mair_el1, x0
    isb

    /* Set TCR_EL1 */
    ldr     x0, =0x0000000200000000
    orr     x0, x0, #(3 << 12)
    orr     x0, x0, #(1 << 10)
    orr     x0, x0, #(1 << 8)
    msr     tcr_el1, x0
    isb

    /* Enable MMU */
    mrs     x0, sctlr_el1
    orr     x0, x0, #0x1
    msr     sctlr_el1, x0
    isb

    /* Enable caches */
    mrs     x0, sctlr_el1
    orr     x0, x0, #0x4
    orr     x0, x0, #0x1000
    msr     sctlr_el1, x0
    isb

    mov     x30, x30_saved
    ret

#endif /* BSP_USING_HP232X_USE_STATIC_ASM - 方案A */

/* HP232X temporary stack in IRAM0 (last 256KB, constraint-compliant) */
    .align 12
    .global hp232x_temp_stack_top
hp232x_temp_stack:
    .space 32768              /* 32KB temp stack in IRAM0 */
hp232x_temp_stack_top:

#endif /* BSP_USING_HP232X */
