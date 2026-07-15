/*
 * biz_subsys.h — HP232x business layer API (init / workers / emmc_biz).
 */
#ifndef BIZ_SUBSYS_H__
#define BIZ_SUBSYS_H__

/**
 * Start emmc_biz on @cpu (0 .. RT_CPUS_NR-1). Idempotent if already running.
 * Ensures drv_emmc_init() has run (needed after BSP_BIZ_SKIP_THREADS).
 * Returns 0 on success.
 */
int biz_emmc_biz_start_on_cpu(int cpu);

/** Convenience: bind like BSP_BIZ_EMMC_ON_CPU1 (cpu1) else cpu0. */
int biz_emmc_biz_start(void);

/** Start flash CPU0 worker (no-op if DIRECT). Safe to call multiple times. */
int biz_flash_worker_ensure(void);

/**
 * Start MCU I2C slave + i2c_mcu / mcu_err threads.
 * Idempotent. Needed after BSP_I2C_DEFER (msh: i2c start).
 */
int biz_i2c_ensure(void);

/** Soft/config-only biz status for msh. */
int biz_emmc_biz_is_running(void);
int biz_emmc_hw_is_ready(void);

#endif /* BIZ_SUBSYS_H__ */
