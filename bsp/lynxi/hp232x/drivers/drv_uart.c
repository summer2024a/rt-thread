/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author         Notes
 * 2020-04-16     bigmagic       first version
 * 2025-06-17     lynxi          hp232x BSP — simplified UART driver
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#include "board.h"
#include "drv_uart.h"
#include <mmu.h>

size_t uart0_addr = 0;

#ifdef RT_USING_UART0
static struct rt_serial_device _serial0;
#endif

struct hw_uart_device
{
    rt_ubase_t hw_base;
    rt_uint32_t irqno;
};

static rt_err_t uart_init(rt_ubase_t hw_base, rt_uint32_t baud_rate)
{
    uint32_t bauddiv = (UART_REFERENCE_CLOCK / baud_rate) * 1000 / 16;
    uint32_t ibrd = bauddiv / 1000;
    unsigned int ier;
    unsigned char c;

    DW_APB_REG_LCR(hw_base) = 0x3; /* 8n1 */
    ier = DW_APB_REG_IER(hw_base);
    DW_APB_REG_IER(hw_base) = (ier & 0x40); /* no interrupt */
    DW_APB_REG_IIR(hw_base) = 0x0;  /* no fifo */
    DW_APB_REG_MCR(hw_base) = 0x3;  /* DTR + RTS */

    c = DW_APB_REG_LCR(hw_base);
    DW_APB_REG_LCR(hw_base) = c | UART_LCR_DLAB;
    DW_APB_REG_RBR(hw_base) = ibrd & 0xff;
    DW_APB_REG_IER(hw_base) = (ibrd >> 8) & 0xff;
    DW_APB_REG_LCR(hw_base) = c & ~UART_LCR_DLAB;

    return RT_EOK;
}

static rt_err_t uart_configure(struct rt_serial_device *serial, struct serial_configure *cfg)
{
    struct hw_uart_device *uart;

    RT_ASSERT(serial != RT_NULL);
    uart = (struct hw_uart_device *)serial->parent.user_data;

    return uart_init(uart->hw_base, cfg->baud_rate);
}

static rt_err_t uart_control(struct rt_serial_device *serial, int cmd, void *arg)
{
    struct hw_uart_device *uart;

    RT_ASSERT(serial != RT_NULL);
    uart = (struct hw_uart_device *)serial->parent.user_data;

    rt_kprintf("[UART_CTRL] cmd=%d, irqno=%d\n", cmd, uart->irqno);

    switch (cmd)
    {
    case RT_DEVICE_CTRL_CLR_INT:
        /* disable rx irq */
        rt_kprintf("[UART_CTRL] Disabling RX interrupt\n");
        DW_APB_REG_IER(uart->hw_base) = (DW_APB_REG_IER(uart->hw_base) & ~0x1);
        rt_hw_interrupt_mask(uart->irqno);
        break;

    case RT_DEVICE_CTRL_SET_INT:
        /* enable rx irq */
        rt_kprintf("[UART_CTRL] Enabling RX interrupt: IER before=0x%x\n", DW_APB_REG_IER(uart->hw_base));
        DW_APB_REG_IER(uart->hw_base) = (DW_APB_REG_IER(uart->hw_base) | 0x1);
        rt_kprintf("[UART_CTRL] IER after=0x%x\n", DW_APB_REG_IER(uart->hw_base));
        rt_hw_interrupt_umask(uart->irqno);
        rt_kprintf("[UART_CTRL] Interrupt umask called for IRQ %d\n", uart->irqno);

        /* DEBUG: Check UART interrupt status */
        unsigned int ier = DW_APB_REG_IER(uart->hw_base);
        rt_kprintf("[UART_CTRL] Final IER=0x%x (bit0=%d)\n", ier, ier & 0x1);
        break;
    }
    return RT_EOK;
}

static int uart_putc(struct rt_serial_device *serial, char c)
{
    struct hw_uart_device *uart;
    RT_ASSERT(serial != RT_NULL);
    uart = (struct hw_uart_device *)serial->parent.user_data;

    while (!(DW_APB_REG_LSR(uart->hw_base) & UART_LSR_THRE));
    DW_APB_REG_RBR(uart->hw_base) = c;

    return 1;
}

static int uart_getc(struct rt_serial_device *serial)
{
    int ch = -1;
    struct hw_uart_device *uart;

    RT_ASSERT(serial != RT_NULL);
    uart = (struct hw_uart_device *)serial->parent.user_data;

    if (DW_APB_REG_LSR(uart->hw_base) & UART_LSR_DR) {
        ch = DW_APB_REG_RBR(uart->hw_base) & 0xff;
    }

    return ch;
}

static const struct rt_uart_ops _uart_ops =
{
    uart_configure,
    uart_control,
    uart_putc,
    uart_getc,
};

volatile void *earlycon_base = 0;
const size_t earlycon_size = 0x1000;

extern void early_putc(int c)
{
    if (c == '\n')
    {
        early_putc('\r');
    }

    while((DW_APB_REG_LSR(earlycon_base) & BOTH_EMPTY) != BOTH_EMPTY);
    DW_APB_REG_RBR(earlycon_base) = c;
}

void rt_hw_earlycon_ioremap_early(void)
{
#ifdef RT_USING_UART0
    rt_ubase_t _earlycon_base = UART0_BASE;
#else
    rt_ubase_t _earlycon_base = UART1_BASE;
#endif

    /* hp232x: identity mapping (no ioremap needed for IRAM-only system) */
    earlycon_base = (void *)_earlycon_base;

    /* init uart - uses physical address before MMU enable */
    uart_init((rt_ubase_t)earlycon_base, 115200);
}

void rt_hw_console_output(const char *str)
{
    if (earlycon_base)
    {
        while (*str)
        {
            early_putc(*str++);
        }
    }
}

void early_printhex(rt_ubase_t number)
{
    char str[sizeof("0123456789abcdef")];

    str[16] = 0;

    for (int i = 15; i >= 0; --i)
    {
        str[i] = "0123456789abcdef"[(number & 0xf)];
        number >>= 4;
    }

    rt_kputs(str);
}

static void rt_hw_uart_isr(int irqno, void *param)
{
    unsigned int iir, status;
    struct rt_serial_device *serial = (struct rt_serial_device *)param;
    struct hw_uart_device *uart = (struct hw_uart_device *)serial->parent.user_data;

    /* DEBUG: Log UART interrupt entry */
    rt_kprintf("[UART_ISR] IRQ=%d triggered\n", irqno);

    iir = DW_APB_REG_IIR(uart->hw_base);
    rt_kprintf("[UART_ISR] IIR=0x%x\n", iir);

    if ((iir & 0x3f) == UART_IIR_RX_TIMEOUT)
    {
        status = DW_APB_REG_LSR(uart->hw_base);
        if (!(status & (UART_LSR_DR | UART_LSR_BI)))
        {
            (void) DW_APB_REG_RX(uart->hw_base);
        }
    }

    if (!(iir & UART_IIR_NO_INT))
    {
        rt_hw_serial_isr(serial, RT_SERIAL_EVENT_RX_IND);
        rt_kprintf("[UART_ISR] Called rt_hw_serial_isr\n");
    }

    if ((iir & UART_IIR_BUSY) == UART_IIR_BUSY) {
        (void) DW_APB_REG_USR(uart->hw_base);
    }
}

#ifdef RT_USING_UART0
static struct hw_uart_device _uart0_device =
{
    UART0_BASE,
    IRQ_UART0,
};
#endif

int rt_hw_uart_init(void)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

#ifdef RT_USING_UART0
    struct hw_uart_device *uart0;
    uart0 = &_uart0_device;

    _serial0.ops    = &_uart_ops;
    _serial0.config = config;

    /* hp232x: identity mapping, no ioremap */
    uart0_addr = UART0_BASE;
    earlycon_base = (void *)uart0_addr;
    uart0->hw_base = uart0_addr;

    /* register UART0 device */
    rt_hw_serial_register(&_serial0, "uart0",
                          RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX,
                          uart0);
    rt_hw_interrupt_install(uart0->irqno, rt_hw_uart_isr, &_serial0, "uart0");
#endif

    return 0;
}
