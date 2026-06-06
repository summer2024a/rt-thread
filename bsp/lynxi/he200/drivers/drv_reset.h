#ifndef __DRV_RESET_H__
#define __DRV_RESET_H__

#include <rtthread.h>

enum lynxi_reset_line
{
    LYNXI_RESET_ETH = 0,
    LYNXI_RESET_I2C0,
    LYNXI_RESET_I2C1,
    LYNXI_RESET_I2C2,
    LYNXI_RESET_I2C3,
    LYNXI_RESET_RTC,
    LYNXI_RESET_DMA,
};

rt_err_t lynxi_reset_assert(enum lynxi_reset_line line);
rt_err_t lynxi_reset_deassert(enum lynxi_reset_line line);
rt_err_t lynxi_reset_pulse(enum lynxi_reset_line line, rt_uint32_t delay_ms);

#endif /* __DRV_RESET_H__ */
