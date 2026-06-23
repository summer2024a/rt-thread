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

/* HP232X early debug output - write single char to UART0 at 0x10006000 */
#ifdef BSP_USING_HP232X_DEBUG_UART  /* Only enable UART output if DEBUG_UART defined */
#ifdef BSP_USING_HP232X
.macro early_uart_init
    /* BL1 already initialized UART, ensure FIFO is cleared */
    ldr     x0, =0x10006000      /* UART0 base */
    /* Clear any pending characters by reading RBR until empty */
1:  ldr     w1, [x0, #0x14]      /* UART_LSR */
    tst     w1, #0x01            /* Check DR (Data Ready) */
    b.eq    2f                   /* No data, done */
    ldr     w1, [x0]             /* Read and discard */
    b       1b
2:
.endm

.macro early_putc_no_wait, char
    /* Direct output without waiting - BL1 UART is ready */
    ldr     x0, =0x10006000
    mov     w1, #\char
    str     w1, [x0]
.endm

.macro early_putc, char
    /* Wait for UART transmitter completely empty (TEMT) */
    ldr     x0, =0x10006000
    ldr     x1, =0x10006014       /* UART LSR address */
1:  ldrb    w2, [x1]              /* Read LSR */
    tst     w2, #0x40             /* Check TEMT bit (bit 6) */
    b.eq    1b                    /* Wait until transmitter empty */
    mov     w2, #\char
    strb    w2, [x0]              /* Write character to THR */
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
#ifdef BSP_USING_HP232X
    /* HP232X: Step 2 - Configure EL3 registers + SCTLR_EL2 + eret */

    /* Check if primary CPU */
    mrs     x0, mpidr_el1
    ldr     x1, =0xff00ffffff      /* MPIDR_ID_BITS */
    tst     x0, x1
    b.ne    1f                      /* secondary cores skip */

    /* Primary CPU only: Full EL3 setup */

    /* Step 2.1: Set SCR_EL3 (boot.S:49-53) */
    mov     x0, #0x30               /* RES1 bits */
    orr     x0, x0, #(1 << 0)       /* NS=1: Non-secure EL1 */
    orr     x0, x0, #(1 << 8)       /* HVC enable */
    orr     x0, x0, #(1 << 10)      /* RW=1: 64-bit EL2 */
    msr     scr_el3, x0

    /* Step 2.2: Disable copro. traps to EL3 (boot.S:55) */
    msr     cptr_el3, xzr

    /* Step 2.3: Set timer frequency (boot.S:75-77) */
    ldr     x0, =31250000           /* cnt_freq value */
    msr     cntfrq_el0, x0

    /* Step 2.4: Set SCTLR_EL2 (spin.S:22-23) */
    ldr     x0, =(3 << 28 | 3 << 22 | 1 << 18 | 1 << 16 | 1 << 11 | 3 << 4)  /* SCTLR_EL2_RESET */
    msr     sctlr_el2, x0

    /* Step 2.5: Set SPSR_EL3 and ELR_EL3 for eret (spin.S:24-26) */
    adr     x0, .hp232x_after_eret   /* Return address after eret */
    mov     x1, #(9 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))  /* SPSR_KERNEL: EL2h + all exceptions masked */
    msr     elr_el3, x0
    msr     spsr_el3, x1

    /* Step 2.6: Execute eret */
    eret

.hp232x_after_eret:
    /* At EL2 now: Call init_cpu_el for EL2→EL1 transition */
    bl      init_cpu_el

    /* After init_cpu_el, we should be at EL1 */
    /* Continue with normal RT-Thread boot */
    b       .hp232x_el1_continue

1:  /* Secondary CPUs: spin wait */
    wfi
    b       1b

.hp232x_el1_continue:
    /* At EL1: Continue normal boot flow */

#endif /* BSP_USING_HP232X - End of HP232X-specific bootwrapper flow */

    mov     dtb_paddr, x0
    mov     boot_arg0, x1
    mov     boot_arg1, x2
    mov     boot_arg2, x3

#ifdef BSP_USING_HP232X
    /* ===== Set stack pointer ===== */
    ldr     stack_top, =.hp232x_temp_stack_top
    mov     sp, stack_top
#endif

    /* Save cpu id temp */
#ifdef ARCH_USING_HW_THREAD_SELF
    msr     tpidrro_el0, xzr
    /* Save thread self */
#endif /* ARCH_USING_HW_THREAD_SELF */
    msr     tpidr_el1, xzr

#ifdef BSP_USING_HP232X
#ifdef BSP_USING_HP232X_DEBUG_UART
    early_putc 'I'              /* Before init_cpu_el */
#endif
#endif
    bl      init_cpu_el

#ifdef BSP_USING_HP232X
#ifdef BSP_USING_HP232X_DEBUG_UART
    early_putc 'B'              /* Before init_kernel_bss */
#endif
#endif
    bl      init_kernel_bss

#ifdef BSP_USING_HP232X
#ifdef BSP_USING_HP232X_DEBUG_UART
    early_putc 'K'              /* Ready for kernel */
#endif
#endif
    bl      init_cpu_stack_early

#ifdef RT_USING_OFW
    /* Save devicetree info */
    mov     x0, dtb_paddr
    bl      rt_hw_fdt_install_early
#endif

    /* Now we are in the end of boot cpu process */
#ifdef BSP_USING_HP232X
    /* HP232X: Skip init_mmu_early, directly jump to rtthread_startup */
    ldr     kernel_entry, =rtthread_startup
    b       kernel_start
#else
    ldr     kernel_entry, =rtthread_startup
    b       init_mmu_early
#endif
    /* never come back */

kernel_start:
    /* Jump to the PE's system entry */
    mov     x29, xzr
    mov     x30, kernel_entry
    br      kernel_entry

cpu_idle:
    wfe
    b       cpu_idle

/* HP232X secondary CPU spin loop (WFE wait for mailbox release) */
#ifdef BSP_USING_HP232X

/* HP232X temporary stack in IRAM0 (first 256KB, constraint-compliant) */
    .section .early_hp232x, "aw", @progbits
    .align 12
.hp232x_temp_stack:
    .space 4096               /* 4KB temp stack in IRAM0 */
.hp232x_temp_stack_top:

/* HP232X secondary CPU spin table - placed in IRAM0 first 256KB (constraint-compliant) */
    .section ".early_hp232x", "aw", @progbits
    .align 12
.hp232x_spin_table:
    .space 64                   /* Spin table for up to 8 CPUs */
.hp232x_spin_table_end:

/* HP232X stack area - placed in IRAM1 last 256KB (constraint-compliant) */
    .section ".early_hp232x", "aw", @progbits  /* Use IRAM0, not .bss */
    .align 4
.hp232x_stack_bottom:
    .space 256                  /* 256 bytes per CPU (bootwrapper STACK_SIZE) */
.hp232x_stack_top:

/* setup_stack function - bootwrapper style (stack.S:22-32) */
setup_stack:
    mov     w1, #256                /* STACK_SIZE = 256 */
    ldr     x2, =.hp232x_stack_top
    umsubl  x0, w0, w1, x2          /* sp = st_base - cpu * st_size */
    mov     sp, x0
    ret

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

    /* Spin table base in IRAM0 (constraint-compliant) */
    ldr     x4, =0x0401FF00         /* mbox_address = IRAM0 spin table */
    add     x4, x4, x5              /* Current CPU's mbox entry */

1:  wfe
    ldr     x1, [x4]                /* Read entry point */
    cbz     x1, 1b                  /* Zero means not released */
    br      x1                       /* Jump to entry point when released */

/* HP232X secondary CPU spin loop end - see above for implementation */

/*
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
    .quad 0x100000601                 /* (4 << 30) | 0x600 | 0x1 */

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

/*
 * ===== 方案B: hp232x_enable_mmu_early（C代码配置） =====
 * 这是当前使用的方案，页表在board.c中定义
 * 此函数配合board.c中的页表配置
 */
hp232x_enable_mmu_early:
    /* 方案B实现（配合board.c的页表配置） */
    /* Note: Page tables are configured in board.c rt_hw_board_init() */
    get_phy x0, .early_tbl0_page
    ldr     x1, =0              /* Clear page table */
    mov     x2, #512
1:  str     x1, [x0], #8
    subs    x2, x2, #1
    b.ne    1b

    /* Set up 3-level identity mapping for IRAM0, IRAM1, GIC */
    get_phy x0, .early_tbl0_page    /* Level 0 (PGD) */
    get_phy x1, .early_tbl1_page    /* Level 1 (PUD) */

    /* PGD[0] -> PUD table for 0-512GB */
    orr     x2, x1, #0x3           /* Table descriptor (attr=0, type=3) */
    str     x2, [x0, #0*8]

    /* PGD[2] -> PUD table for 4GB-5GB */
    add     x3, x1, #4096          /* Second PUD table at offset 4KB */
    orr     x2, x3, #0x3
    str     x2, [x0, #2*8]

    /* PUD[0]: IRAM0 + GIC area (VA 0x04000000-0x07FFFFFF, 1GB) */
    /* Identity mapping: PA base = 0x04000000 */
    /* Covers IRAM0 (0x04000000, 64MB) and GIC (0x08000000, 128MB) */
    /* Note: Boot ROM at 0x00000000-0x03FFFFFF is NOT mapped (PA 0x00000000 removed) */

    /* PUD[1] (0x40000000-0x7FFFFFFF, 1GB): IRAM0 - Normal cacheable */
    /* CRITICAL FIX: IRAM0 is at 0x04000000 (64MB), map pud_table[0] instead */
    ldr     x2, =0x04000000        /* IRAM0 CORRECT address (64MB, not 1GB!) */
    orr     x2, x2, #(1 << 10)     /* AF=1 */
    orr     x2, x2, #(2 << 8)      /* SH=Outer Shareable */
    orr     x2, x2, #0x1           /* Block descriptor, MA=0 default */
    str     x2, [x1, #0*8]         /* pud_table[0] maps IRAM0 at 0x04000000 */

    /* pud_table[0] maps IRAM0 at 0x04000000 (64MB) */
    /* This covers VA 0x04000000-0x07FFFFFF (1GB block including IRAM0, GIC) */

    /* GIC is at 0x08000000 (128MB) - also in PUD[0] range */
    /* Since PUD[0] PA=0x04000000, GIC VA 0x08000000 maps to PA 0x08000000 ✓ */

    /* No need for separate PUD entry - covered by 1GB block */

    /* PUD_hi[4] (0x100000000-0x103FFFFFFF, 1GB): IRAM1 - Normal cacheable */
    add     x3, x1, #4096          /* Second PUD table */
    ldr     x2, =0x100000000       /* IRAM1 physical base */
    orr     x2, x2, #(1 << 10)     /* AF=1 */
    orr     x2, x2, #(2 << 8)      /* SH=Outer Shareable */
    orr     x2, x2, #0x1           /* Block descriptor, MA=0 default */
    str     x2, [x3, #4*8]

    /* Set TTBR0_EL1 to our page table */
    get_phy x0, .early_tbl0_page
    msr     ttbr0_el1, x0
    isb

    /* Configure MAIR_EL1 (Memory Attribute Indirection Register) */
    ldr     x0, =0x00447fUL        /* RT-Thread standard: Attr0=WB, Attr2=Device */
    msr     mair_el1, x0
    isb

    /* Configure TCR_EL1 for 4KB pages, IPS=2 (40-bit PA, supports IRAM1 at 4GB+) */
    ldr     x0, =0x0000000200000000UL  /* IPS=2 (bits[34:32]=2), TG0=4KB, T0SZ=0 */
    msr     tcr_el1, x0
    isb

    /* Enable MMU and caches */
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)      /* M=1: MMU enable */
    orr     x0, x0, #(1 << 2)      /* C=1: Data cache enable */
    orr     x0, x0, #(1 << 12)     /* I=1: Instruction cache enable */
    msr     sctlr_el1, x0
    isb

    ret

#endif /* BSP_USING_HP232X - 方案B: hp232x_enable_mmu_early */

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
    ldr     x19, =rt_hw_secondary_cpu_bsp_start
    b       enable_mmu_early
#endif /* RT_USING_SMP */

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
	mrs 	x0, S3_4_C12_C9_5
	orr	    x0, x0, #(1 << 0)	    /* Set ICC_SRE_EL2.SRE==1 */
	orr	    x0, x0, #(1 << 3)	    /* Set ICC_SRE_EL2.Enable==1 */
	msr 	S3_4_C12_C9_5, x0
	isb					        /*  Make sure SRE is now set */
#endif /* BSP_USING_GICV3 */

    mov     x0, #(1 << 31)          /* Enable AArch64 in EL1 */
    orr     x0, x0, #(1 << 1)       /* SWIO hardwired */
    msr     hcr_el2, x0

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
    get_phy x1, __bss_start
    get_phy x2, __bss_end
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
 * Early page builtin - HP232X: Move to IRAM0 for MMU access
 */
#ifdef BSP_USING_HP232X
    /* HP232X: Page tables in IRAM1 is OK - accessible before MMU */
    .section ".bss.noclean.early_page"
#else
    .section ".bss.noclean.early_page"
#endif

    /* ===== CRITICAL: HP232X MMU early tables MUST be in IRAM0 ===== */
    /* Problem: These tables were in IRAM1 (>4GB), inaccessible before MMU */
    /* Solution: Place in IRAM0 .text section so CPU can access them */
#ifdef BSP_USING_HP232X
    .section .text, "ax", @progbits  /* Force into IRAM0 .text */
#endif
    .align 12
.early_tbl0_page:
    .space ARCH_PAGE_SIZE
.early_tbl1_page:
#ifdef BSP_USING_HP232X
    /* HP232X: Only need 2 * 4KB for PUD tables */
    .space 2 * ARCH_PAGE_SIZE
#else
    /* Map 4G -> 2M * 512 entries */
    .space 4 * ARCH_PAGE_SIZE
#endif

/* early_page_array - Linker needs this symbol even if HP232X doesn't use it */
#ifdef BSP_USING_HP232X
    .section ".early_hp232x", "aw", @progbits  /* HP232X: Place in IRAM0 */
    .align 12
.early_page_array:
    .space 4096  /* Minimal placeholder for HP232X */
#else
    .section ".bss", "aw", @nobits  /* Standard: .bss is uninitialized */
    .align 12
.early_page_array:
    .space 24 * ARCH_PAGE_SIZE, 0  /* Explicitly zero-filled in .bss */
#endif
