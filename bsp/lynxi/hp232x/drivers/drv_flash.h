/*
 * drv_flash.h — KA200 SPI NOR flash driver for RT-Thread HP232x BSP.
 * Ported from hp640_arm/common/spl/spl_cmd.c flash sections.
 */

#ifndef DRV_FLASH_H__
#define DRV_FLASH_H__

#include <stdint.h>

#define DRV_FLASH_EMMC_ERROR_ADDR   0x000E7000UL

/*
 * hp640 spl.c: CONFIG_SPL_640_HEAD @ 0x64000 — SPI XIP bootcode below this.
 * hp640 spl_cmd.c: FLASH_ADDR_TEST @ 0x900000 for safe read/write debug.
 */
#define DRV_FLASH_XIP_BOOTCODE_END  0x00064000UL
#define DRV_FLASH_TEST_ADDR         0x00900000UL

#define DRV_FLASH_SECTOR_SIZE       (64 * 1024)
#define DRV_FLASH_SUBSECTOR_SIZE    4096
#define DRV_FLASH_PAGE_SIZE         256

/* Soft init (INIT_DEVICE): no SSI MMIO. */
int drv_flash_init(void);
/* Leave-XIP + bind + JEDEC — call from main() after scheduler/SMP up. */
int drv_flash_bringup(void);
/* Manual bring-up when BSP_FLASH_DEFER_INIT (msh step debug). */
void drv_flash_dbg_status(void);
uint32_t drv_flash_dbg_ssi_ctrl(int do_write, uint32_t val);
void drv_flash_dbg_open_flash(void);   /* inv + B8 + 98 */
void drv_flash_dbg_bind(void);         /* set regs from strap, no SSI MMIO */
int drv_flash_dbg_peek(void);          /* read CTRL0/SR — may SError */
int drv_flash_dbg_jedec(void);         /* JEDEC after bind+leave */
int drv_flash_dbg_init_now(void);      /* same as drv_flash_bringup */
#if defined(RT_USING_SMP)
int drv_flash_worker_start(void);
#endif
int drv_flash_is_known(void);
uint32_t drv_flash_get_jedec(void);
int drv_flash_bootcode_overlap(uint32_t addr, uint32_t len, int include_erase);
int drv_flash_read(void *dst, int len, uint32_t addr);
int drv_flash_write(const void *src, int len, uint32_t addr);
int drv_flash_erase(uint32_t addr, int len);
/* Core1 SError triage: direct SSI on cpu_id (no bounce). */
int drv_flash_ssi_cpu_probe(int cpu_id);

#endif /* DRV_FLASH_H__ */
