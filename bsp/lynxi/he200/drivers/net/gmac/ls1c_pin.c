#include "ls1c_pin.h"

#define LYNXI_PINCTRL_BASE      0x12000000U
#define LYNXI_PINCFG0_OFFSET    0x44U
#define LYNXI_PINCFG_STEP       0x4U
#define LYNXI_PINMUX_BIT        (1U << 10)
#define LYNXI_MAX_PIN_NUM       120U

static inline volatile unsigned int *lynxi_pincfg_reg(unsigned int gpio)
{
    return (volatile unsigned int *)(LYNXI_PINCTRL_BASE + LYNXI_PINCFG0_OFFSET + gpio * LYNXI_PINCFG_STEP);
}

void pin_set_purpose(unsigned int gpio, pin_purpose_t purpose)
{
    volatile unsigned int *reg;
    unsigned int value;

    if (gpio >= LYNXI_MAX_PIN_NUM)
    {
        return;
    }

    reg = lynxi_pincfg_reg(gpio);
    value = *reg;

    if (purpose == PIN_PURPOSE_GPIO)
    {
        value |= LYNXI_PINMUX_BIT;
    }
    else
    {
        value &= ~LYNXI_PINMUX_BIT;
    }

    *reg = value;
}

void pin_set_remap(unsigned int gpio, pin_remap_t remap)
{
    (void)gpio;
    (void)remap;
}
