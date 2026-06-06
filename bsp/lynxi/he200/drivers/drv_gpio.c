#include <rtthread.h>
#include <rthw.h>
#include <drivers/dev_pin.h>
#include "lynxi.h"

#ifdef RT_USING_PIN

#define LYNXI_GPIO_PORT_COUNT        4U
#define LYNXI_GPIO_PINS_PER_PORT     32U
#define LYNXI_GPIO_MAX_PIN           (LYNXI_GPIO_PORT_COUNT * LYNXI_GPIO_PINS_PER_PORT)

#define LYNXI_GPIO_SWPORTA_DR        0x00U
#define LYNXI_GPIO_SWPORTA_DDR       0x04U
#define LYNXI_GPIO_EXT_PORTA         0x50U
#define LYNXI_GPIO_PORT_STRIDE       0x0CU
#define LYNXI_PINCTRL_BASE           0x12000000U
#define LYNXI_PINCFG0_OFFSET         0x44U
#define LYNXI_PINCFG_STEP            0x4U
#define LYNXI_PINMUX_GPIO_BIT        (1U << 10)

rt_inline rt_bool_t lynxi_gpio_pin_valid(rt_base_t pin)
{
    return (pin >= 0) && ((rt_uint32_t)pin < LYNXI_GPIO_MAX_PIN);
}

rt_inline rt_uint32_t lynxi_gpio_port(rt_base_t pin)
{
    return ((rt_uint32_t)pin / LYNXI_GPIO_PINS_PER_PORT);
}

rt_inline rt_uint32_t lynxi_gpio_bit(rt_base_t pin)
{
    return (1U << ((rt_uint32_t)pin % LYNXI_GPIO_PINS_PER_PORT));
}

rt_inline volatile rt_uint32_t *lynxi_gpio_reg(rt_uint32_t offset)
{
    return (volatile rt_uint32_t *)(GPIO_BASE + offset);
}

rt_inline volatile rt_uint32_t *lynxi_pincfg_reg(rt_base_t pin)
{
    return (volatile rt_uint32_t *)(rt_uintptr_t)(LYNXI_PINCTRL_BASE + LYNXI_PINCFG0_OFFSET + (rt_uint32_t)pin * LYNXI_PINCFG_STEP);
}

static void lynxi_pin_mode(struct rt_device *device, rt_base_t pin, rt_uint8_t mode)
{
    rt_uint32_t port;
    rt_uint32_t bit;
    volatile rt_uint32_t *ddr_reg;
    rt_base_t level;

    RT_UNUSED(device);

    if (!lynxi_gpio_pin_valid(pin))
    {
        return;
    }

    port = lynxi_gpio_port(pin);
    bit = lynxi_gpio_bit(pin);
    ddr_reg = lynxi_gpio_reg(LYNXI_GPIO_SWPORTA_DDR + port * LYNXI_GPIO_PORT_STRIDE);

    level = rt_hw_interrupt_disable();
    *lynxi_pincfg_reg(pin) |= LYNXI_PINMUX_GPIO_BIT;
    if (mode == PIN_MODE_OUTPUT)
    {
        *ddr_reg |= bit;
    }
    else
    {
        *ddr_reg &= ~bit;
    }
    rt_hw_interrupt_enable(level);
}

static void lynxi_pin_write(struct rt_device *device, rt_base_t pin, rt_uint8_t value)
{
    rt_uint32_t port;
    rt_uint32_t bit;
    volatile rt_uint32_t *dr_reg;
    rt_base_t level;

    RT_UNUSED(device);

    if (!lynxi_gpio_pin_valid(pin))
    {
        return;
    }

    port = lynxi_gpio_port(pin);
    bit = lynxi_gpio_bit(pin);
    dr_reg = lynxi_gpio_reg(LYNXI_GPIO_SWPORTA_DR + port * LYNXI_GPIO_PORT_STRIDE);

    level = rt_hw_interrupt_disable();
    if (value == PIN_LOW)
    {
        *dr_reg &= ~bit;
    }
    else
    {
        *dr_reg |= bit;
    }
    rt_hw_interrupt_enable(level);
}

static rt_ssize_t lynxi_pin_read(struct rt_device *device, rt_base_t pin)
{
    rt_uint32_t port;
    rt_uint32_t bit;
    volatile rt_uint32_t *ext_reg;

    RT_UNUSED(device);

    if (!lynxi_gpio_pin_valid(pin))
    {
        return PIN_LOW;
    }

    port = lynxi_gpio_port(pin);
    bit = lynxi_gpio_bit(pin);
    ext_reg = lynxi_gpio_reg(LYNXI_GPIO_EXT_PORTA + port * LYNXI_GPIO_PORT_STRIDE);

    return ((*ext_reg & bit) ? PIN_HIGH : PIN_LOW);
}

static rt_err_t lynxi_pin_attach_irq(struct rt_device *device, rt_base_t pin,
                                     rt_uint8_t mode, void (*hdr)(void *args), void *args)
{
    RT_UNUSED(device);
    RT_UNUSED(pin);
    RT_UNUSED(mode);
    RT_UNUSED(hdr);
    RT_UNUSED(args);
    return -RT_ENOSYS;
}

static rt_err_t lynxi_pin_detach_irq(struct rt_device *device, rt_base_t pin)
{
    RT_UNUSED(device);
    RT_UNUSED(pin);
    return -RT_ENOSYS;
}

static rt_err_t lynxi_pin_irq_enable(struct rt_device *device, rt_base_t pin, rt_uint8_t enabled)
{
    RT_UNUSED(device);
    RT_UNUSED(pin);
    RT_UNUSED(enabled);
    return -RT_ENOSYS;
}

static const struct rt_pin_ops _lynxi_pin_ops =
{
    lynxi_pin_mode,
    lynxi_pin_write,
    lynxi_pin_read,
    lynxi_pin_attach_irq,
    lynxi_pin_detach_irq,
    lynxi_pin_irq_enable,
    RT_NULL,
};

int rt_hw_pin_init(void)
{
    return rt_device_pin_register("pin", &_lynxi_pin_ops, RT_NULL);
}
INIT_BOARD_EXPORT(rt_hw_pin_init);

#endif
