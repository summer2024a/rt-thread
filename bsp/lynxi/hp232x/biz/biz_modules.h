/*
 * biz_modules.h — HP232X business module compile switches (from Kconfig/rtconfig.h).
 * Disable all but one module when bringing up incrementally.
 */
#ifndef BIZ_MODULES_H__
#define BIZ_MODULES_H__

#include <rtconfig.h>

#ifdef BSP_DRV_MOD_EXEC_APU
#define BIZ_MOD_EXEC_APU        1
#endif

#ifdef BSP_DRV_MOD_EXEC_MISC
#define BIZ_MOD_EXEC_MISC       1
#endif

#ifdef BSP_DRV_MOD_EXEC_STRESS
#define BIZ_MOD_EXEC_STRESS       1
#endif

#ifdef BSP_DRV_MOD_EXEC_SELFTEST
#define BIZ_MOD_EXEC_SELFTEST   1
#endif

#ifdef BSP_DRV_MOD_EMMC_DLL
#define BIZ_MOD_EMMC_DLL        1
#endif

#ifdef BSP_DRV_MOD_PCIE
#define BIZ_MOD_PCIE            1
#endif

#ifdef BSP_DRV_MOD_FLASH_UPGRADE
#define BIZ_MOD_FLASH_UPGRADE   1
#endif

#ifdef BSP_DRV_MOD_FINSH
#define BIZ_MOD_FINSH           1
#endif

#endif /* BIZ_MODULES_H__ */
