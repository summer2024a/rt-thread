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


#define APB_UART_BASE_OFFSET      (0x10006000)
//pl011 offset
#define APB_UART0_BASE_OFFSET     (0x10006000)
#define APB_UART1_BASE_OFFSET     (0x10007000)

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

/* UART PL011 */
#define UART_BASE                   (PER_BASE + APB_UART_BASE_OFFSET)
//extern uint32_t uart_base_addr;
#define UART0_BASE                  (UART_BASE + 0x0)
#define UART1_BASE                  (UART_BASE + 0x1000)
#define IRQ_AUX_UART                (96 + 29)
#define UART_REFERENCE_CLOCK        (50000000)

#define IRQ_UART0                   (32 + 25)
#define IRQ_UART1                   (32 + 26)

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
#define GIC_PL500_REDISTRIBUTOR_PPTR    0x080a0000
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
#define ST_BASE_OFFSET     (0x003000)
#define STIMER_BASE  (PER_BASE  + ST_BASE_OFFSET)
extern size_t stimer_base_addr;
#define STIMER_CS    __REG32(stimer_base_addr + 0x0000)
#define STIMER_CLO   __REG32(stimer_base_addr + 0x0004)
#define STIMER_CHI   __REG32(stimer_base_addr + 0x0008)
#define STIMER_C0    __REG32(stimer_base_addr + 0x000C)
#define STIMER_C1    __REG32(stimer_base_addr + 0x0010)
#define STIMER_C2    __REG32(stimer_base_addr + 0x0014)
#define STIMER_C3    __REG32(stimer_base_addr + 0x0018)

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

//mac
#define MAC_REG                 (void *)(0xfd580000)
extern uint8_t *                mac_reg_base_addr;

#define ETH_IRQ                 (160+29)

#define SEND_DATA_NO_CACHE      (0x08200000)
extern uint8_t *                eth_send_no_cache;

#define RECV_DATA_NO_CACHE      (0x08400000)
extern uint8_t *                eth_recv_no_cache;

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
