/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2023-02-06     RT-Thread      first version
 */
#ifndef __LYNXI_H__
#define __LYNXI_H__

#include <rtthread.h>

#define __REG32(x)  (*((volatile unsigned int *)(x)))
#define __REG16(x)  (*((volatile unsigned short *)(x)))

/* GIC IRQ MAX */
#define MAX_HANDLERS                (256)

/* base address */
#define PER_BASE                    (0x0)

//gpio offset
#define GPIO_BASE_OFFSET            (0x1000E000)

//uart offset
#define APB_UART_BASE_OFFSET      (0x10006000)
#define APB_UART0_BASE_OFFSET     (0x10006000)
#define APB_UART1_BASE_OFFSET     (0x10007000)

//sdio offset
#define SDIO_BASE_OFFSET            (0x10040000)

//reset controller offset
#define RST_CTRL_BASE_OFFSET        (0x12500000)
#define I2C0_BASE_OFFSET            (0x10002000)
#define I2C1_BASE_OFFSET            (0x10003000)
#define I2C2_BASE_OFFSET            (0x10004000)
#define I2C3_BASE_OFFSET            (0x10005000)
#define RTC_BASE_OFFSET             (0x10014000)
#define DW_AXI_DMA_BASE_OFFSET      (0x1001A000)
/* Synopsys GMAC / dwmac — same PA as Linux lynchip-lite-base.dtsi ethernet@10020000 */
#define GMAC_BASE_OFFSET            (0x10020000)
#define CPR_BASE_OFFSET             (0x12500000)
#define EFUSE_BASE_OFFSET           (0x12300000)

/* GPIO */
#define GPIO_BASE_ADDR              (PER_BASE + GPIO_BASE_OFFSET)
extern size_t gpio_base_addr;
#define GPIO_BASE                   (gpio_base_addr)
#define GPIO_IRQ_NUM                (3)   //40 pin mode
#define IRQ_GPIO0                   (96 + 49) //bank0 (0 to 27)
#define IRQ_GPIO1                   (96 + 50) //bank1 (28 to 45)
#define IRQ_GPIO2                   (96 + 51) //bank2 (46 to 57)
#define IRQ_GPIO3                   (96 + 52) //bank3

/* Timer (ARM side) */
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

/* UART */
#define UART_BASE                   (PER_BASE + APB_UART_BASE_OFFSET)
#define UART0_BASE                  (UART_BASE + 0x0)
#define UART1_BASE                  (UART_BASE + 0x1000)
#define UART_REFERENCE_CLOCK        (50000000)
#define IRQ_UART0                   (32 + 25)
#define IRQ_UART1                   (32 + 26)

/* Linux lynchip-lite-base.dtsi ethernet@10020000: interrupts = <0 78 4> */
#define IRQ_GMAC                    78

/* I2C */
#define I2C0_BASE                   (PER_BASE + I2C0_BASE_OFFSET)
#define I2C1_BASE                   (PER_BASE + I2C1_BASE_OFFSET)
#define IRQ_I2C0                    (32 + 31)
#define IRQ_I2C1                    (32 + 28)

/* Reset controller */
#define RST_CTRL_BASE               (PER_BASE + RST_CTRL_BASE_OFFSET)

/* SDIO */
#define SDIO_BASE                   (PER_BASE + SDIO_BASE_OFFSET)

#define I2C2_BASE                   (PER_BASE + I2C2_BASE_OFFSET)
#define I2C3_BASE                   (PER_BASE + I2C3_BASE_OFFSET)

/* RTC */
#define RTC_BASE                    (PER_BASE + RTC_BASE_OFFSET)

/* CPR / EFUSE / DMA */
#define CPR_BASE                    (PER_BASE + CPR_BASE_OFFSET)
#define EFUSE_BASE                  (PER_BASE + EFUSE_BASE_OFFSET)
#define DW_AXI_DMA_BASE             (PER_BASE + DW_AXI_DMA_BASE_OFFSET)
#define GMAC_BASE                   (PER_BASE + GMAC_BASE_OFFSET)

// 0x40, 0x44, 0x48, 0x4c: Core 0~3 Timers interrupt control
#define CORE0_TIMER_IRQ_CTRL        HWREG32(0x08600000)
#define TIMER_IRQ                   30
#define NON_SECURE_TIMER_IRQ        (1 << 1)

/* GIC */
#define INTC_BASE                   (0x08000000)
#define ARM_GIC_NR_IRQS             (512)
#define ARM_GIC_MAX_NR              (512)
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

//watchdog
#define WDT_BASE        (PER_BASE + 0x10010000)
extern size_t         wdt_base_addr;
#define PM_RSTC         HWREG32(wdt_base_addr + 0x1c)
#define PM_RSTS         HWREG32(wdt_base_addr + 0x20)
#define PM_WDOG         HWREG32(wdt_base_addr + 0x24)

#define PM_PASSWORD                 (0x5A000000)
#define PM_WDOG_TIME_SET            (0x000fffff)
#define PM_RSTS_HADWRH_SET          (0x00000040)
#define PM_RSTC_WRCFG_FULL_RESET    (0x00000020)
#define PM_RSTC_WRCFG_CLR           (0xffffffcf)
#define PM_RSTC_RESET               (0x00000102)

//timer
#define CONFIG_TIMER_NUM    4
#define ST_BASE_OFFSET     (0x10012000)
#define STIMER_BASE  (PER_BASE  + ST_BASE_OFFSET)
#define DRV_ERRNO_TIMER_BASE STIMER_BASE
#define TIMER_IRQ_START    (66)
extern size_t stimer_base_addr;

//pcie ep
#define PCIE_EP_BASE_OFFSET     (0x1a000000)
#define PCIE_EP_BASE (PER_BASE + PCIE_EP_BASE_OFFSET)
extern size_t pcie_ep_base_addr;

#define DELAY_MICROS(micros)                            \
    do{                                                 \
        rt_uint32_t compare = STIMER_CLO + micros * 25; \
        while (STIMER_CLO < compare);                   \
    } while (0)

//External Mass Media Controller (SD Card)
#define MMC0_BASE_ADDR    (PER_BASE+0x300000)
extern size_t mmc0_base_addr;
#define MMC2_BASE_ADDR    (PER_BASE+0x340000)
extern size_t mmc2_base_addr;

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

#endif
