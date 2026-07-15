/*
 * drv_pvt.h — KA200 PVT read-only driver (TS / VM / PD).
 * Ported from hp640_arm/common/spl/spl_pvt.c (read path only).
 */

#ifndef DRV_PVT_H__
#define DRV_PVT_H__

#include <stdint.h>

#define DRV_PVT_TS_COUNT    4
#define DRV_PVT_VM_COUNT    16
#define DRV_PVT_APU_VM_COUNT 7
#define DRV_PVT_PD_COUNT    4

/* Read sensor values. init=0 on first call, init=1 to reuse cached config. */
int drv_pvt_read_temperature(float *result, uint32_t init);
int drv_pvt_read_voltage(float *result, uint32_t init);
int drv_pvt_read_apu_vm(float *result, uint32_t init);
int drv_pvt_read_process_detector(float *floop, float *gdelay,
                                  uint32_t startno, uint32_t num);

#endif /* DRV_PVT_H__ */
