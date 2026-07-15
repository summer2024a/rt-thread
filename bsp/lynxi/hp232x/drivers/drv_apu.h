/*
 * drv_apu.h — KA200 APU control driver for RT-Thread HP232x BSP.
 * Ported from hp640_arm/common/spl/spl_cmd.c APU sections.
 */

#ifndef DRV_APU_H__
#define DRV_APU_H__

#include <stdint.h>

#define DRV_APU_CTRL_REG            0x12500074UL

#define DRV_APU_CTRL_RST            0x01U
#define DRV_APU_CTRL_CLK            0x02U
#define DRV_APU_CTRL_FRQ            0x04U

#define DRV_APU_STAT_OFFSET         0x0018C1CCUL
#define DRV_APU_MDBG_CTRL_START     0x0018854CUL
#define DRV_APU_MDBG_CTRL_MODE      0x00188550UL
#define DRV_APU_MDBG_ADDR           0x00188554UL
#define DRV_APU_MDBG_WDATA          0x00188558UL

void drv_apu_reset(void);
int drv_apu_enable(int enable);
int drv_apu_pll_set(uint32_t pll_cfg2);
int drv_apu_init(void);

uint32_t drv_apu_read32(uint32_t offset);
void drv_apu_write32(uint32_t offset, uint32_t value);
int drv_apu_read_status(void);

#endif /* DRV_APU_H__ */
