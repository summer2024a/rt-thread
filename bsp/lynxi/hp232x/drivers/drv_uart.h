/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 * 2020-05-26     bigmagic       add other uart
 * 2025-06-17     lynxi          hp232x BSP — simplified UART driver
 */

#ifndef DRV_UART_H__
#define DRV_UART_H__

/*
 * These are the definitions for the Line Status Register
 */
#define UART_LSR_DR     0x01    /* Data ready */
#define UART_LSR_OE     0x02    /* Overrun */
#define UART_LSR_PE     0x04    /* Parity error */
#define UART_LSR_FE     0x08    /* Framing error */
#define UART_LSR_BI     0x10    /* Break */
#define UART_LSR_THRE   0x20    /* Xmit holding register empty */
#define UART_LSR_TEMT   0x40    /* Xmitter empty */
#define UART_LSR_ERR    0x80    /* Error */

#define BOTH_EMPTY (UART_LSR_TEMT | UART_LSR_THRE)

/*
 * These are the definitions for the Line Control Register
 */
#define UART_LCR_WLS_MSK 0x03
#define UART_LCR_WLS_8   0x03    /* 8 bit character length */
#define UART_LCR_STB     0x04
#define UART_LCR_PEN     0x08
#define UART_LCR_EPS     0x10
#define UART_LCR_STKP    0x20
#define UART_LCR_SBRK    0x40
#define UART_LCR_DLAB    0x80

#define UART_IIR_NO_INT      0x01
#define UART_IIR_BUSY        0x07
#define UART_IIR_RX_TIMEOUT  0x0c

#define UART_RX      (0x00)
#define UART_RBR     (0x00)
#define UART_IER     (0x04)
#define UART_IIR     (0x08)
#define UART_LCR     (0x0c)
#define UART_MCR     (0x10)
#define UART_LSR     (0x14)
#define UART_MSR     (0x18)
#define UART_SCR     (0x1c)
#define UART_USR     (0x7c)

#define DW_APB_REG_RX(BASE)             HWREG32(BASE + UART_RX)
#define DW_APB_REG_RBR(BASE)            HWREG32(BASE + UART_RBR)
#define DW_APB_REG_LCR(BASE)            HWREG32(BASE + UART_LCR)
#define DW_APB_REG_IER(BASE)            HWREG32(BASE + UART_IER)
#define DW_APB_REG_IIR(BASE)            HWREG32(BASE + UART_IIR)
#define DW_APB_REG_LSR(BASE)            HWREG32(BASE + UART_LSR)
#define DW_APB_REG_USR(BASE)            HWREG32(BASE + UART_USR)
#define DW_APB_REG_MSR(BASE)            HWREG32(BASE + UART_MSR)
#define DW_APB_REG_MCR(BASE)            HWREG32(BASE + UART_MCR)

int rt_hw_uart_init(void);
void rt_hw_earlycon_ioremap_early(void);

#endif /* DRV_UART_H__ */
