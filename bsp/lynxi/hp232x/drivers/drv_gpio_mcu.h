/*
 * drv_gpio_mcu.h — GPIO pulse to MCU on eMMC error (GPIO 78).
 */

#ifndef DRV_GPIO_MCU_H__
#define DRV_GPIO_MCU_H__

void drv_gpio_mcu_error_init(void);
void drv_gpio_mcu_error_pulse(void);

#endif /* DRV_GPIO_MCU_H__ */
