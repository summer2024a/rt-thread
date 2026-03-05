/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 */

#ifndef DRV_UART_H__
#define DRV_UART_H__

/*
 * These are the definitions for the Line Status Register
 */
#define UART_LSR_DR	0x01		/* Data ready */
#define UART_LSR_OE	0x02		/* Overrun */
#define UART_LSR_PE	0x04		/* Parity error */
#define UART_LSR_FE	0x08		/* Framing error */
#define UART_LSR_BI	0x10		/* Break */
#define UART_LSR_THRE	0x20		/* Xmit holding register empty */
#define UART_LSR_TEMT	0x40		/* Xmitter empty */
#define UART_LSR_ERR	0x80		/* Error */

#define UART_MSR_DCD	0x80		/* Data Carrier Detect */
#define UART_MSR_RI		0x40		/* Ring Indicator */
#define UART_MSR_DSR	0x20		/* Data Set Ready */
#define UART_MSR_CTS	0x10		/* Clear to Send */
#define UART_MSR_DDCD	0x08		/* Delta DCD */
#define UART_MSR_TERI	0x04		/* Trailing edge ring indicator */
#define UART_MSR_DDSR	0x02		/* Delta DSR */
#define UART_MSR_DCTS	0x01		/* Delta CTS */

#define BOTH_EMPTY (UART_LSR_TEMT | UART_LSR_THRE)

/*
 * These are the definitions for the Line Control Register
 *
 * Note: if the word length is 5 bits (UART_LCR_WLEN5), then setting
 * UART_LCR_STOP will select 1.5 stop bits, not 2 stop bits.
 */
#define UART_LCR_WLS_MSK 0x03		/* character length select mask */
#define UART_LCR_WLS_5	0x00		/* 5 bit character length */
#define UART_LCR_WLS_6	0x01		/* 6 bit character length */
#define UART_LCR_WLS_7	0x02		/* 7 bit character length */
#define UART_LCR_WLS_8	0x03		/* 8 bit character length */
#define UART_LCR_STB	0x04		/* # stop Bits, off=1, on=1.5 or 2) */
#define UART_LCR_PEN	0x08		/* Parity eneble */
#define UART_LCR_EPS	0x10		/* Even Parity Select */
#define UART_LCR_STKP	0x20		/* Stick Parity */
#define UART_LCR_SBRK	0x40		/* Set Break */
#define UART_LCR_BKSE	0x80		/* Bank select enable */
#define UART_LCR_DLAB	0x80		/* Divisor latch access bit */

#define UART_IIR_NO_INT     0x01    /* No interrupts pending */
#define UART_IIR_BUSY       0x07    /* DesignWare APB Busy Detect */
#define UART_IIR_RX_TIMEOUT 0x0c    /* OMAP RX Timeout interrupt */

#define UART_RX                 (0x00)
#define UART_RBR                (0x00)
#define UART_IER                (0x04)
#define UART_IIR                (0x08)
#define UART_LCR                (0x0c)
#define UART_MCR                (0x10)
#define UART_LSR                (0x14)
#define UART_MSR                (0x18)
#define UART_SCR                (0x1c)
#define UART_USR                (0x7c)

#define DW_APB_REG_RX(BASE)             HWREG32(BASE + UART_RX)
#define DW_APB_REG_RBR(BASE)            HWREG32(BASE + UART_RBR)
#define DW_APB_REG_LCR(BASE)            HWREG32(BASE + UART_LCR)
#define DW_APB_REG_IER(BASE)            HWREG32(BASE + UART_IER)
#define DW_APB_REG_IIR(BASE)            HWREG32(BASE + UART_IIR)
#define DW_APB_REG_LSR(BASE)            HWREG32(BASE + UART_LSR)
#define DW_APB_REG_USR(BASE)            HWREG32(BASE + UART_USR)
#define DW_APB_REG_MSR(BASE)            HWREG32(BASE + UART_MSR)
#define DW_APB_REG_SCR(BASE)            HWREG32(BASE + UART_SCR)
#define DW_APB_REG_MCR(BASE)            HWREG32(BASE + UART_MCR)


int rt_hw_uart_init(void);
void rt_hw_earlycon_ioremap_early(void);
void rt_hw_ioremap_after_mmu(void);

#endif /* DRV_UART_H__ */
