/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Date           Author       Notes
 * 2020-01-15     bigmagic     the first version
 * 2020-08-10     SummerGift   support clang compiler
 * 2023-04-29     GuEe-GUI     support kernel's ARM64 boot header
 * 2024-01-18     Shell        fix implicit dependency of cpuid management
 */

#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif

#include <mmu.h>
#include <rtconfig.h>

#define ARM64_IMAGE_FLAG_BE_SHIFT           0
#define ARM64_IMAGE_FLAG_PAGE_SIZE_SHIFT    (ARM64_IMAGE_FLAG_BE_SHIFT + 1)
#define ARM64_IMAGE_FLAG_PHYS_BASE_SHIFT    (ARM64_IMAGE_FLAG_PAGE_SIZE_SHIFT + 2)

#define ARM64_IMAGE_FLAG_LE                 0
#define ARM64_IMAGE_FLAG_BE                 1
#define ARM64_IMAGE_FLAG_PAGE_SIZE_4K       1
#define ARM64_IMAGE_FLAG_PAGE_SIZE_16K      2
#define ARM64_IMAGE_FLAG_PAGE_SIZE_64K      3
#define ARM64_IMAGE_FLAG_PHYS_BASE          1

#define _HEAD_FLAG(field)                   (_HEAD_FLAG_##field << ARM64_IMAGE_FLAG_##field##_SHIFT)

#ifdef ARCH_CPU_BIG_ENDIAN
#define _HEAD_FLAG_BE                       ARM64_IMAGE_FLAG_BE
#else
#define _HEAD_FLAG_BE                       ARM64_IMAGE_FLAG_LE
#endif
#define _HEAD_FLAG_PAGE_SIZE                ((ARCH_PAGE_SHIFT - 10) / 2)
#define _HEAD_FLAG_PHYS_BASE                1

#define _HEAD_FLAGS                         (_HEAD_FLAG(BE) | _HEAD_FLAG(PAGE_SIZE) | _HEAD_FLAG(PHYS_BASE))

.macro get_phy, reg, symbol
    adrp    \reg, \symbol
    add     \reg, \reg, #:lo12:\symbol
.endm

.macro get_pvoff, tmp, out
    ldr     \tmp, =.boot_cpu_stack_top
    get_phy \out, .boot_cpu_stack_top
    sub     \out, \out, \tmp
.endm

/* HP232X early debug output - optimized UART interface */
/*
 * Architecture:
 *   - entry_point.S: Define early_putc macro (uses x6/x7)
 *   - pre_entry.S: Initialize x6/x7 registers, call early_putc
 *
 * Register usage:
 *   - x6: UART LSR address (pre-initialized by pre_entry.S)
 *   - x7: UART THR address (pre-initialized by pre_entry.S)
 *   - x8: Temporary register for character and LSR polling
 *
 * CRITICAL: early_putc depends on x6/x7 being initialized by pre_entry.S!
 */
#ifdef BSP_USING_HP232X_DEBUG_UART
#ifdef BSP_USING_HP232X
.macro early_uart_init
    /* BL1 already initialized UART hardware */
    /* pre_entry.S will initialize x6/x7 registers */
.endm

.macro early_putc_no_wait, char
    /* Direct output without waiting - use pre-initialized x7 */
    mov     w8, #\char
    str     w8, [x7]              /* Write to THR (x7 pre-loaded by pre_entry.S) */
.endm

.macro early_putc, char
ldr     x6, =0x10006000         /* UART0 base address (THR) */
1:  ldr     w8, [x6, #0x14]      /* 读 LSR = base + 0x14 */
    tst     w8, #0x20
    b.eq    1b
    mov     w8, \char
    str     w8, [x6]             /* 写 THR = base + 0 */
.endm

.macro early_putc_reg, reg
    /* Output register value as hex digit (低4位) */
    ldr     x6, =0x10006000         /* UART0 base address */
1:  ldr     w8, [x6, #0x14]         /* 读 LSR */
    tst     w8, #0x20
    b.eq    1b
    and     x8, \reg, #0xf           /* 取低4位 (使用x寄存器) */
    cmp     x8, #9
    b.ls    2f
    add     x8, x8, #'A' - 10        /* 10-15 输出 A-F */
    b       3f
2:  add     x8, x8, #'0'             /* 0-9 输出 0-9 */
3:  str     w8, [x6]                 /* 写 THR (使用w寄存器) */
.endm

/* Bootwrapper-style definitions for EL transition */
.macro drop_el_to_el2 mode addr
    msr     elr_el3, \addr
    msr     spsr_el3, \mode
    eret
.endm

#else
.macro early_uart_init
.endm
.macro early_putc_no_wait, char
.endm
.macro early_putc, char
.endm
.macro drop_el_to_el2 mode addr
.endm
#endif  /* BSP_USING_HP232X */

#else  /* BSP_USING_HP232X_DEBUG_UART not defined - no UART output */
.macro early_uart_init
.endm
.macro early_putc_no_wait, char
.endm
.macro early_putc, char
.endm
.macro drop_el_to_el2 mode addr
.endm
#endif  /* BSP_USING_HP232X_DEBUG_UART */

    .section ".text.entrypoint","ax"

#ifdef RT_USING_OFW
/*
 * Our goal is to boot the rt-thread as possible without modifying the
 * bootloader's config, so we use the kernel's boot header for ARM64:
 *   https://www.kernel.org/doc/html/latest/arch/arm64/booting.html#call-the-kernel-image
 */
_head:
    b       _start          /* Executable code */
    .long   0               /* Executable code */
    .quad   _text_offset    /* Image load offset from start of RAM, little endian */
    .quad   _end - _head    /* Effective Image size, little endian (_end defined in link.lds) */
    .quad   _HEAD_FLAGS     /* Kernel flags, little endian */
    .quad   0               /* Reserved */
    .quad   0               /* Reserved */
    .quad   0               /* Reserved */
    .ascii  "ARM\x64"       /* Magic number */
    .long   0               /* Reserved (used for PE COFF offset) */
#endif /* RT_USING_OFW */

/* Variable registers: x21~x28 */
dtb_paddr .req x21
boot_arg0 .req x22
boot_arg1 .req x23
boot_arg2 .req x24
stack_top .req x25
kernel_entry .req x19

    .global _start
_start:
/*
 * Boot CPU general-purpose register settings:
 *   x0 = physical address of device tree blob (dtb) in system RAM.
 *   x1 = 0 (reserved for future use)
 *   x2 = 0 (reserved for future use)
 *   x3 = 0 (reserved for future use)
 */

/* Early full cache maintenance for HP232X to prevent SError on first eret/context switch.
 * Must be done before any stack/BSS access or MMU-dependent code. */
    ic      ialluis
    dsb     sy
    isb

    /* Save cpu stack */
    get_phy stack_top, .boot_cpu_stack_top

#ifdef BSP_USING_HP232X_BL22
    /* BL22 mode: bootcode/bootwrapper already initialized UART, GIC, CCI.
     * We are currently at EL1 (bootwrapper did EL3→EL1 transition).
     * Stack already set up by bootwrapper.
     * Skip bootwrapper init, go directly to common RT-Thread initialization. */
#elif defined(BSP_USING_HP232X_BL21)
    /* ===== IRAM1 unified stack @ 0x10007FFF0 (92KB contiguous area) ===== */
    /* HP232X CPU supports 40-bit PA — proven by bootwrapper using 0x100020000.
     * .boot_cpu_stack_top is in .bss.noclean.cpus_stack (0x100050000-0x10005a000).
     * BSS section starts at 0x10005a000 (after noclean), so stack won't be zeroed. */
    mov     sp, stack_top
    isb

    /* HP232X: Call platform-specific bootwrapper initialization */
    bl      hp232x_bootwrapper_init
    /* After bootwrapper init, continue to common RT-Thread initialization */
#else
    /* Standard RT-Thread boot flow for other platforms */
#endif

    /* ===== Common RT-Thread initialization (all platforms) ===== */

    mov     dtb_paddr, x0
    mov     boot_arg0, x1
    mov     boot_arg1, x2
    mov     boot_arg2, x3

    /* Save cpu id temp */
#ifdef ARCH_USING_HW_THREAD_SELF
    msr     tpidrro_el0, xzr
    /* Save thread self */
#endif /* ARCH_USING_HW_THREAD_SELF */
    msr     tpidr_el1, xzr

#ifdef BSP_USING_HP232X
    /* ===== HP232X IPS验证：检查默认PA范围 ===== */
    /* 读取ID_AA64MMFR0_EL1.PARange，确认是否支持40-bit PA */
    early_putc 'P'     /* Print: checking PARange */
    mrs     x10, ID_AA64MMFR0_EL1
    and     x10, x10, #0xf    /* PARange in bits [3:0] */
    early_putc_reg x10  /* 输出PARange值 */

    /* 读取当前TCR_EL1.IPS值 */
    mrs     x11, TCR_EL1
    lsr     x11, x11, #32     /* IPS在bit[34:32] */
    and     x11, x11, #7      /* 3-bit IPS field */
    early_putc 'I'     /* IPS */
    early_putc_reg x11
    early_putc '\n'
#endif

    early_putc 'I'
    bl      init_cpu_el

    early_putc 'B'
    bl      init_kernel_bss

    early_putc 'K'
    bl      init_cpu_stack_early

    early_putc 'S'

#ifdef RT_USING_OFW
    /* Save devicetree info */
    mov     x0, dtb_paddr
    bl      rt_hw_fdt_install_early
#endif

    /* Now we are in the end of boot cpu process */
#ifdef BSP_USING_HP232X
    /* HP232X: Skip init_mmu_early, directly jump to rtthread_startup */
    ldr     kernel_entry, =rtthread_startup
    early_putc 'U'
    b       kernel_start
#else
    ldr     kernel_entry, =rtthread_startup
    b       init_mmu_early
#endif
    /* never come back */

kernel_start:
    /* Jump to the PE's system entry */
    /* Final cache maintenance before jumping to rtthread_startup (prevents SError on first eret) */
    ic      ialluis
    dsb     sy
    isb
    mov     x29, xzr
    mov     x30, kernel_entry
    early_putc 'E'
    br      kernel_entry

cpu_idle:
    wfe
    b       cpu_idle

#ifdef RT_USING_SMP
    .globl _secondary_cpu_entry
_secondary_cpu_entry:
#ifdef RT_USING_OFW
    /* Read cpu id */
    mrs     x5, mpidr_el1
    ldr     x1, =rt_cpu_mpidr_table
    get_pvoff x4 x2
    add     x1, x1, x2
    mov     x2, #0
    ldr     x4, =0xff00ffffff
    and     x0, x5, x4

.cpu_id_confirm:
    add     x2, x2, #1              /* Next cpu id inc */
    ldr     x3, [x1], #8
    cmp     x3, #0
    beq     cpu_idle
    and     x3, x3, x4
    cmp     x3, x0
    bne     .cpu_id_confirm

    /* Save this mpidr */
    str     x5, [x1, #-8]

    /* Get cpu id success */
    sub     x0, x2, #1
#endif /* RT_USING_OFW */
    /* Save cpu id global */
    bl      rt_hw_cpu_id_set
    bl      rt_hw_cpu_id

    /* Set current cpu's stack top */
    sub     x0, x0, #1
    mov     x1, #ARCH_SECONDARY_CPU_STACK_SIZE
    get_phy x2, .secondary_cpu_stack_top
    msub    stack_top, x0, x1, x2

    bl      init_cpu_el
    bl      init_cpu_stack_early

    /* secondary cpu start to startup */
#ifdef BSP_USING_HP232X
    /* HP232X: Skip enable_mmu_early (uses generic .early_tbl0_page).
     * board.c:rt_hw_secondary_cpu_bsp_start() calls hp232x_mmu_secondary_init()
     * which sets hp232x_mmu_l1 and enables MMU correctly.
     * See doc/mmu_design.md section 14 for details. */
    ldr     kernel_entry, =rt_hw_secondary_cpu_bsp_start
    br      kernel_entry
#else
    ldr     kernel_entry, =rt_hw_secondary_cpu_bsp_start
    b       enable_mmu_early
#endif
#endif /* RT_USING_SMP */

/* ===== Common initialization functions (all platforms) ===== */

init_cpu_el:
    mrs     x0, CurrentEL           /* CurrentEL Register. bit 2, 3. Others reserved */
    lsr     x0, x0, #2
    and     x0, x0, #3

    /* running at EL3? */
    cmp     x0, #3
    bne     .init_cpu_hyp_test

    /* should never be executed, just for completeness. (EL3) */
    mov     x1, #(1 << 0)           /* EL0 and EL1 are in Non-Secure state */
    orr     x1, x1, #(1 << 4)       /* RES1 */
    orr     x1, x1, #(1 << 5)       /* RES1 */
    orr     x1, x1, #(1 << 10)      /* The next lower level is AArch64 */
    msr     scr_el3, x1

    mov     x1, #9                  /* Next level is 0b1001->EL2h */
    orr     x1, x1, #(1 << 6)       /* Mask FIQ */
    orr     x1, x1, #(1 << 7)       /* Mask IRQ */
    orr     x1, x1, #(1 << 8)       /* Mask SError */
    orr     x1, x1, #(1 << 9)       /* Mask Debug Exception */
    msr     spsr_el3, x1

    get_phy x1, .init_cpu_hyp
    msr     elr_el3, x1
    eret

.init_cpu_hyp_test:
    /* running at EL2? */
    cmp     x0, #2                  /* EL2 = 0b10  */
    bne     .init_cpu_sys

.init_cpu_hyp:
    /* Enable CNTP for EL1 */
    mrs     x0, cnthctl_el2         /* Counter-timer Hypervisor Control register */
    orr     x0, x0, #(1 << 0)       /* Don't traps NS EL0/1 accesses to the physical counter */
    orr     x0, x0, #(1 << 1)       /* Don't traps NS EL0/1 accesses to the physical timer */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

#ifdef BSP_USING_GICV3
    mrs     x0, S3_4_C12_C9_5
    orr     x0, x0, #(1 << 0)       /* Set ICC_SRE_EL2.SRE==1 */
    orr     x0, x0, #(1 << 3)       /* Set ICC_SRE_EL2.Enable==1 */
    msr     S3_4_C12_C9_5, x0
    isb                     /*  Make sure SRE is now set */

    /* CRITICAL: Set ICC_SRE_EL1 from EL2 to enable EL1 GICv3 system register access */
    mov     x0, #((1 << 2) | (1 << 1) | (1 << 0))  /* DIB=1, DFB=1, SRE=1 */
    msr     S3_0_C12_C12_5, x0    /* ICC_SRE_EL1 */
    isb
#endif /* BSP_USING_GICV3 */

    /* Configure HCR_EL2: Route all interrupts to EL1 */
    mov     x0, #(1 << 31)          /* RW=1: Enable AArch64 in EL1 */
    orr     x0, x0, #(1 << 1)       /* SWIO=1: SWIO hardwired */
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

    mov     x0, #5                  /* Next level is 0b0101->EL1h */
    orr     x0, x0, #(1 << 6)       /* Mask FIQ */
    orr     x0, x0, #(1 << 7)       /* Mask IRQ */
    orr     x0, x0, #(1 << 8)       /* Mask SError */
    orr     x0, x0, #(1 << 9)       /* Mask Debug Exception */
    msr     spsr_el2, x0

    get_phy x0, .init_cpu_sys
    msr     elr_el2, x0
    eret

.init_cpu_sys:
    mrs     x0, sctlr_el1
    bic     x0, x0, #(3 << 3)       /* Disable SP Alignment check */
    bic     x0, x0, #(1 << 1)       /* Disable Alignment check */
    msr     sctlr_el1, x0

    mrs     x0, cntkctl_el1
    orr     x0, x0, #(1 << 1)      /* Set EL0VCTEN, enabling the EL0 Virtual Count Timer */
    msr     cntkctl_el1, x0

    /* Avoid trap from SIMD or float point instruction */
    mov     x0, #0x00300000         /* Don't trap any SIMD/FP instructions in both EL0 and EL1 */
    msr     cpacr_el1, x0

    /* Applying context change */
    dsb     ish
    isb

    ret

init_kernel_bss:
    /* HP232X FIX: Use ldr to load absolute addresses instead of adrp-based get_phy.
     * BSS is in IRAM1 at 0x10005a000 (>4GB, bit 32 set). adrp can only encode 33-bit
     * addresses, so get_phy macro fails. ldr has no such limitation.
     * This ensures BSS is zeroed BEFORE rtthread_startup calls rt_hw_spin_lock_init,
     * which initializes _cpus_lock (a BSS variable used by SMP).
     *
     * Original issue: entry_point.S skipped BSS zeroing for high addresses,
     * but rtthread_startup initialized spinlocks BEFORE rt_hw_board_init zeroed BSS.
     * This caused _cpus_lock to contain garbage values, breaking SMP boot.
     */
    ldr     x1, =__bss_start        /* Load absolute address (no adrp limitation) */
    ldr     x2, =__bss_end
    sub     x2, x2, x1              /* Get bss size */

    and     x3, x2, #7              /* x3 is < 7 */
    ldr     x4, =~0x7
    and     x2, x2, x4              /* Mask ~7 */

.clean_bss_loop_quad:
    cbz     x2, .clean_bss_loop_byte
    str     xzr, [x1], #8
    sub     x2, x2, #8
    b       .clean_bss_loop_quad

.clean_bss_loop_byte:
    cbz     x3, .clean_bss_end
    strb    wzr, [x1], #1
    sub     x3, x3, #1
    b       .clean_bss_loop_byte

.clean_bss_end:
    ret

init_cpu_stack_early:
    msr     spsel, #1
    mov     sp, stack_top

    ret

init_mmu_early:
    get_phy x0, .early_page_array
    bl      set_free_page

    get_phy x0, .early_tbl0_page
    get_phy x1, .early_tbl1_page

    get_pvoff x2 x3
    ldr     x2, =ARCH_EARLY_MAP_SIZE    /* Map 1G memory for kernel space */
    bl      rt_hw_mem_setup_early

    b       enable_mmu_early

enable_mmu_early:
    get_phy x0, .early_tbl0_page
    get_phy x1, .early_tbl1_page

    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x1
    dsb     sy

    bl      mmu_tcr_init

    /*
     * OK, now, we don't use sp before jump to kernel, set sp to current cpu's
     * stack top to visual address
     */
    get_pvoff x1 x0
    mov     x1, stack_top
    sub     x1, x1, x0
    mov     sp, x1

    ldr     x30, =kernel_start      /* Set LR to kernel_start function, it's virtual addresses */

    /* Enable page table translation */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 12)      /* Stage 1 instruction access Cacheability control */
    orr     x1, x1, #(1 << 2)       /* Cacheable Normal memory in stage1 */
    orr     x1, x1, #(1 << 0)       /* MMU Enable */
    msr     sctlr_el1, x1

    dsb     ish
    isb

    ic      ialluis     /* Invalidate all instruction caches in Inner Shareable domain to Point of Unification */
    dsb     ish
    isb

    tlbi    vmalle1     /* Invalidate all stage 1 translations used at EL1 with the current VMID */
    dsb     ish
    isb

    ret

/*
 * CPU stack builtin
 */
    .section ".bss.noclean.cpus_stack"
    .align 12
.cpus_stack:
#if defined(RT_USING_SMP) && RT_CPUS_NR > 1
    .space (ARCH_SECONDARY_CPU_STACK_SIZE * (RT_CPUS_NR - 1))
#endif
.secondary_cpu_stack_top:
    .space ARCH_SECONDARY_CPU_STACK_SIZE
.boot_cpu_stack_top:

/*
 * Early page builtin
 */
    .section ".bss.noclean.early_page"
    .align 12
.early_tbl0_page:
    .space ARCH_PAGE_SIZE
.early_tbl1_page:
    /* Map 4G -> 2M * 512 entries */
    .space 4 * ARCH_PAGE_SIZE
#ifdef BSP_USING_HP232X
    /* HP232X doesn't use dynamic page allocation (board.c builds its own MMU tables).
     * Shrink from 24 pages (96KB) to 4 pages (16KB) — just enough for fallback. */
.early_page_array:
    .space 4 * ARCH_PAGE_SIZE
#else
.early_page_array:
    .space 24 * ARCH_PAGE_SIZE
#endif

