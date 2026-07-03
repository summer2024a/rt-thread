/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2025-06-17     lynxi          hp232x BSP for KA200, direct boot from bootcode
 */

#ifndef __LYNXI_H__
#define __LYNXI_H__

#include <rtthread.h>

#define __REG32(x)  (*((volatile unsigned int *)(x)))
#define __REG16(x)  (*((volatile unsigned short *)(x)))

/* GIC IRQ MAX */
#define MAX_HANDLERS                (128)

/*
 * HP232X / KA200 memory layout (two IRAM segments only, no external DDR):
 *
 * IRAM0: 0x04000000 ~ 0x0403FFFF  (前256KB)
 *   BL2_BOOT: kernel text + data + mmu_table (0x04000020)
 *   BL3_BOOT: reserved for bootwrapper + SPL
 * IRAM0: 0x04040000 ~ 0x0407FFFF  (后256KB)
 *   BL2_BOOT: reserved
 *   BL3_BOOT: kernel text + data + mmu_table (0x04040020)
 *
 * IRAM1: 0x100000000 ~ 0x10003FFFF (前256KB保留) — 不可使用
 * IRAM1: 0x100040000 ~ 0x10007FFFF (后256KB可用) — stack + heap + page pool
 *
 * IRAM1 layout:
 *   0x100040000        CPU stacks + early data (~64KB)
 *   0x100050000        .bss section start
 *   0x100071000        page pool start (16KB)
 *   0x100075000        heap start (32KB)
 *   0x10007FFFC        stack top (IRAM1_STACK_TOP, grows downward)
 *
 * The chip boots from bootcode in IRAM0, then jumps to rt-thread.
 * Bootwrapper (pre_entry.S) initializes GICv3 + EL3→EL2→EL1.
 * No U-Boot SPL is used — bootwrapper directly boots RT-Thread kernel.
 */

/* base address — peripherals sit at 0x08000000 (same interconnect as HE200) */
#define PER_BASE                    (0x0)

/* gpio offset */
#define GPIO_BASE_OFFSET            (0x1000E000)

/* uart offset */
#define APB_UART_BASE_OFFSET        (0x10006000)
#define APB_UART0_BASE_OFFSET       (0x10006000)
#define APB_UART1_BASE_OFFSET       (0x10007000)

/* reset controller offset */
#define RST_CTRL_BASE_OFFSET        (0x12500000)

/* GPIO */
#define GPIO_BASE_ADDR              (PER_BASE + GPIO_BASE_OFFSET)
extern size_t gpio_base_addr;
#define GPIO_BASE                   (gpio_base_addr)

/* UART */
#define UART_BASE                   (PER_BASE + APB_UART_BASE_OFFSET)
#define UART0_BASE                  (UART_BASE + 0x0)
#define UART1_BASE                  (UART_BASE + 0x1000)
#define UART_REFERENCE_CLOCK        (50000000)
#define IRQ_UART0                   (32 + 25)
#define IRQ_UART1                   (32 + 26)

/* GIC */
#define INTC_BASE                   (0x08000000)
#define ARM_GIC_NR_IRQS             (512)
#define ARM_GIC_MAX_NR              (1)
#define GIC_V2_BASE                 (INTC_BASE)
extern size_t gic_base_addr;
#define GIC_V2_DISTRIBUTOR_BASE     (gic_base_addr + 0x0)
#define GIC_V2_CPU_INTERFACE_BASE   (gic_base_addr + 0x100000)
#define GIC_V2_HYPERVISOR_BASE      (gic_base_addr + 0x4000)
#define GIC_V2_VIRTUAL_CPU_BASE     (gic_base_addr + 0x6000)

#define GIC_PL400_DISTRIBUTOR_PPTR  GIC_V2_DISTRIBUTOR_BASE
#define GIC_PL400_CONTROLLER_PPTR   GIC_V2_CPU_INTERFACE_BASE

#define GIC_IRQ_START   0
#define GIC_ACK_INTID_MASK  0x000003ff

/* GICv3 */
#define GIC_PL500_DISTRIBUTOR_PPTR      GIC_PL400_DISTRIBUTOR_PPTR
#define GIC_PL500_REDISTRIBUTOR_PPTR    0x08100000
#define GIC_PL500_CONTROLLER_PPTR       GIC_PL400_CONTROLLER_PPTR
#define GIC_PL500_ITS_PPTR              0x08020000

/* Timer */
#define ARM_TIMER_IRQ       (64)
extern size_t arm_timer_base;
#define ARM_TIMER_BASE      (PER_BASE + 0xB000)
#define ARM_TIMER_LOAD      HWREG32(arm_timer_base + 0x400)
#define ARM_TIMER_VALUE     HWREG32(arm_timer_base + 0x404)
#define ARM_TIMER_CTRL      HWREG32(arm_timer_base + 0x408)
#define ARM_TIMER_IRQCLR    HWREG32(arm_timer_base + 0x40C)
#define ARM_TIMER_RAWIRQ    HWREG32(arm_timer_base + 0x410)
#define ARM_TIMER_MASKIRQ   HWREG32(arm_timer_base + 0x414)
#define ARM_TIMER_RELOAD    HWREG32(arm_timer_base + 0x418)
#define ARM_TIMER_PREDIV    HWREG32(arm_timer_base + 0x41C)
#define ARM_TIMER_CNTR      HWREG32(arm_timer_base + 0x420)

/* Core 0~3 Timers interrupt control */
#define CORE0_TIMER_IRQ_CTRL        HWREG32(0x08600000)
#define TIMER_IRQ                   30
#define NON_SECURE_TIMER_IRQ        (1 << 1)

/* Reset controller */
#define RST_CTRL_BASE               (PER_BASE + RST_CTRL_BASE_OFFSET)

/* watchdog */
#define WDT_BASE        (PER_BASE + 0x10010000)
extern size_t wdt_base_addr;
#define PM_RSTC         HWREG32(wdt_base_addr + 0x1c)
#define PM_RSTS         HWREG32(wdt_base_addr + 0x20)
#define PM_WDOG         HWREG32(wdt_base_addr + 0x24)

#define PM_PASSWORD                 (0x5A000000)
#define PM_WDOG_TIME_SET            (0x000fffff)
#define PM_RSTS_HADWRH_SET          (0x00000040)
#define PM_RSTC_WRCFG_FULL_RESET    (0x00000020)
#define PM_RSTC_WRCFG_CLR           (0xffffffcf)
#define PM_RSTC_RESET               (0x00000102)

/* System timer */
#define CONFIG_TIMER_NUM    4
#define ST_BASE_OFFSET      (0x10012000)
#define STIMER_BASE         (PER_BASE + ST_BASE_OFFSET)
#define TIMER_IRQ_START     (66)  /* DW APB timer0 SPI: 32 + 34 */
extern size_t stimer_base_addr;

/* the basic constants and interfaces needed by gic */
rt_inline rt_ubase_t platform_get_gic_dist_base(void)
{
#ifdef BSP_USING_GICV2
    return GIC_PL400_DISTRIBUTOR_PPTR;
#else
    return GIC_PL500_DISTRIBUTOR_PPTR;
#endif
}

rt_inline rt_ubase_t platform_get_gic_redist_base(void)
{
    return GIC_PL500_REDISTRIBUTOR_PPTR;
}

rt_inline rt_ubase_t platform_get_gic_cpu_base(void)
{
#ifdef BSP_USING_GICV2
    return GIC_PL400_CONTROLLER_PPTR;
#else
    return GIC_PL500_CONTROLLER_PPTR;
#endif
}

rt_inline rt_ubase_t platform_get_gic_its_base(void)
{
    return GIC_PL500_ITS_PPTR;
}

#endif /* __LYNXI_H__ */
