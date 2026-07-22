/*
 * drv_gpio_mcu.c — GPIO78 error interrupt to MCU.
 */

#include <rtthread.h>
#include "drv_gpio_mcu.h"
#include "biz_log.h"
#include "lynxi.h"

#define GPIO_EMMC_ERROR_INTERRUPT   78
#define IOCFG_TEST                  0x12000044U
#define IOCFG_STEP                  0x4U
#define GPIO_IOC_CFG_VAL            0x608U
/* Synopsys DW APB GPIO — same as hp640 dwapb_gpio.c (NOT 0x1000 stride) */
#define GPIO_SWPORT_DR(bank)        (0x00U + (uint32_t)(bank) * 0x0CU)
#define GPIO_SWPORT_DDR(bank)       (0x04U + (uint32_t)(bank) * 0x0CU)
#define GPIO_EXT_PORT_OFFSET(bank)  (0x50U + (bank) * 4U)

static int s_gpio_inited;

static inline uint32_t gpio_readl(uint32_t offset)
{
    return *(volatile uint32_t *)(GPIO_BASE + offset);
}

static inline void gpio_writel(uint32_t val, uint32_t offset)
{
    *(volatile uint32_t *)(GPIO_BASE + offset) = val;
}

void drv_gpio_mcu_error_init(void)
{
    uint32_t bank = GPIO_EMMC_ERROR_INTERRUPT / 32U;
    uint32_t pin = GPIO_EMMC_ERROR_INTERRUPT % 32U;
    uint32_t addr = IOCFG_TEST + IOCFG_STEP * GPIO_EMMC_ERROR_INTERRUPT;
    uint32_t ddr;

    if (s_gpio_inited)
        return;

    *(volatile uint32_t *)(uintptr_t)addr = GPIO_IOC_CFG_VAL;

    ddr = gpio_readl(GPIO_SWPORT_DDR(bank));
    ddr |= (1U << pin);
    gpio_writel(ddr, GPIO_SWPORT_DDR(bank));

    gpio_writel(1U << pin, GPIO_SWPORT_DR(bank));
    s_gpio_inited = 1;
    BIZ_INFO("GPIO%d MCU error interrupt init\n", GPIO_EMMC_ERROR_INTERRUPT);
}

void drv_gpio_mcu_error_pulse(void)
{
    uint32_t bank = GPIO_EMMC_ERROR_INTERRUPT / 32U;
    uint32_t pin = GPIO_EMMC_ERROR_INTERRUPT % 32U;
    uint32_t mask = 1U << pin;

    if (!s_gpio_inited)
        drv_gpio_mcu_error_init();

    gpio_writel(0, GPIO_SWPORT_DR(bank));
    rt_thread_mdelay(500);
    gpio_writel(mask, GPIO_SWPORT_DR(bank));
}
