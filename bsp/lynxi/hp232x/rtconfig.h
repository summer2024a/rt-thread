#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

/* HP232X BSP - two segment IRAM KA200 */
#define BSP_USING_HP232X

/* Boot mode: BL21 or BL22 (mutually exclusive)
 * BL21: kernel runs in first 256KB of IRAM0 (0x04000020), IRAM1 last 256KB
 * BL22: kernel runs in last 256KB of IRAM0 (0x04040020), IRAM1 last 256KB */
#define BSP_USING_HP232X_UARTSETUP_BL22

/* Enable UART debug for boot process tracking */
#define BSP_USING_HP232X_DEBUG_UART

/* Boot init: print all PVT TS/VM channels (bring-up only).
 * Heartbeat still reads PVT via drv_emmc_biz when disabled. */
/* #define BSP_DRV_PVT_BOOT_SAMPLE */

/* Temporary: show BIZ_INFO on UART (heart-beat, emmc_biz, thread start). */
#define BSP_BIZ_LOG_BOOT_INFO
/* #define BSP_BIZ_LOG_LOCATION */   /* 开：每条日志都带 func:line；默认仅 DEBUG 带 */
/* #define BSP_BIZ_LOG_TIMESTAMP */  /* 开：每条日志带 [sec.us] 启动相对时间戳（调试用） */
#define BSP_BIZ_PHASE_STATS        /* 开：静默累计 query/cmd 耗时；msh: phase / phase reset */
/* A/B: hp640-style cached SPL BSS + flush_cache (disable NC dma_nocache arena) */
/* #define BSP_EMMC_DMA_CACHED_BSS */
/* Optional: force HS400 SDCLK_DC=0x3c (hp640 CONFIG_HP640_CUSTOM_EMMC_DC); default uses efuse KA200M=0x21 / KA200=0x23 */
/* #define BSP_EMMC_CUSTOM_DC */

/* Drop mm_anon/mm_fault + MM debug shell cmds; stub private-map APIs. */
#define BSP_HP232X_MM_MINIMAL

/* hp640 business modules — disable all but one for incremental debug */
#define BSP_DRV_MOD_EXEC_APU
#define BSP_DRV_MOD_EXEC_MISC
#define BSP_DRV_MOD_EXEC_STRESS
#define BSP_DRV_MOD_EXEC_SELFTEST
/* #define BSP_DRV_MOD_EMMC_DLL */
/* #define BSP_DRV_MOD_PCIE */
#define BSP_DRV_MOD_FLASH_UPGRADE
#define BSP_DRV_MOD_FINSH

/*
 * Manual bring-up (cold-boot / isolation) — msh hand-start only when defined:
 * - BSP_FLASH_DEFER_INIT: no flash soft/JEDEC/worker at boot;
 *     msh: flash init | flash worker  (compiled only if this macro is on)
 * - BSP_BIZ_SKIP_THREADS: no emmc_biz auto-start (i2c independently: BSP_I2C_DEFER);
 *     msh: biz start|upgrade         (compiled only if this macro is on)
 * - BSP_I2C_DEFER: no i2c_mcu / mcu_err auto-start;
 *     msh: i2c start                 (compiled only if this macro is on)
 * - BSP_SMP_DEFER_SECONDARY:
 *     defined   → 从核仅 msh「smp start / release」
 *     undefined → main 内 leave-XIP+flush+release（自启动）
 *
 * MCU note (HP2320): 早期 Host 查地址后复位会卡 I2C，曾默认开 BSP_I2C_DEFER。
 * 现 I2C 联调已通，默认关 DEFER → boot 自启；隔离调试再临时打开。
 * Production: flash/biz/smp/i2c 均关 DEFER（见 BIZ_PORTING）。
 */
/* #define BSP_FLASH_DEFER_INIT */ /* 开则 flash 命令行 init/worker；关则 boot 自启 */
/* #define BSP_BIZ_SKIP_THREADS */ /* 开则 biz 命令行 start/upgrade；关则 boot 自启 */
/* #define BSP_I2C_DEFER */        /* 开则 i2c 手启（msh i2c start）；关则 boot 自启 */
/* #define BSP_SMP_DEFER_SECONDARY */ /* 开则命令行启动从核；关则 boot 自启动 */

/* emmc_biz 默认绑 CPU1（auto-start / biz_emmc_biz_start） */
#define BSP_BIZ_EMMC_ON_CPU1

/*
 * Flash SSI execution (SMP):
 * - BSP_FLASH_CPU0_WORKER (default): bounce read/write/erase to CPU0 flash worker.
 * - BSP_FLASH_DIRECT_ON_CALLER: run on calling CPU (emmc_biz@CPU1). Trial only.
 * Worker auto-starts in biz_worker_init when BSP_FLASH_DEFER_INIT unset.
 */
#define BSP_FLASH_CPU0_WORKER
/* #define BSP_FLASH_DIRECT_ON_CALLER */ /* trial OK 2026-07-14: FlashWrite on cpu1; keep worker */


/*
 * IRAM1 low 256KB (0x100000000..0x10003FFFF) — BL22: no RTT code/stack.
 * Host Load/upgrade scratch (ka200 DDR_IRAM_ADDR).
 * - defined:   map Normal NC (DMA/cross-CPU coherent; lighter flash path)
 * - undefined: leave Normal WB (use invalidate + page bounce; proven 32× OK)
 */
#define BSP_IRAM1_LOW_NC

/*
 * IRAM0 low 256KB (0x04000000..0x0403FFFF) — BL22: boot-wrapper / Host biz
 * descriptors & buffers (not RTT .text). Map Normal NC like IRAM1 scratch.
 * With BSP_IRAM1_LOW_NC: defines BSP_BIZ_SKIP_HOST_DCACHE — CRC/Store/report
 * dcache ops are not compiled in.
 * Do not enable on BL21 (kernel lives in this half).
 */
#define BSP_IRAM0_LOW_NC

/* Enable components init debugging */
/* Disable other debug outputs */
/* #define RT_USING_DEBUG */
/* #define RT_DEBUGING_ASSERT */

/* Use simple spin table for secondary CPUs */
/* CRITICAL: This macro disables full IRQ handler, causing interrupts to fail */
/* DISABLE this to enable proper interrupt handling */
/* #define BSP_USING_HP232X_SPIN_TABLE */

/* Enable rtdbg LOG_X macros (rtdbg.h); does not enable RT_DEBUGING_ASSERT. */
#define DBG_ENABLE

/* RT-Thread Kernel */

/* klibc options — tiny vsnprintf + long long (%llx/%llu); no float printf */
#define RT_KLIBC_USING_VSNPRINTF_LONGLONG
/* #define RT_KLIBC_USING_VSNPRINTF_STANDARD */ /* off: use rt_vsnprintf_tiny (saves ~IRAM0) */
/* #define RT_KLIBC_USING_VSNPRINTF_DECIMAL_SPECIFIERS */
/* #define RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE 32 */ /* only for STANDARD */
/* end of rt_vsnprintf options */

/* rt_vsscanf options */
/* No %f/%e/%g → do not pull Newlib strtod (hp232x: use hp_fmt for PVT print). */
#define RT_KLIBC_USING_VSSCANF_NO_FLOAT
/* end of rt_vsscanf options */

/* rt_memset options */

/* end of rt_memset options */

/* rt_memcpy options */

/* end of rt_memcpy options */

/* rt_memmove options */

/* end of rt_memmove options */

/* rt_memcmp options */

/* end of rt_memcmp options */

/* rt_strstr options */

/* end of rt_strstr options */

/* rt_strcasecmp options */

/* end of rt_strcasecmp options */

/* rt_strncpy options */

#define RT_KLIBC_USING_LIBC_STRNCPY
/* end of rt_strncpy options */

/* rt_strcpy options */

/* end of rt_strcpy options */

/* rt_strncmp options */

/* end of rt_strncmp options */

/* rt_strcmp options */

/* end of rt_strcmp options */

/* rt_strlen options */

/* end of rt_strlen options */

/* rt_strnlen options */

/* end of rt_strnlen options */
/* end of klibc options */
#define RT_NAME_MAX 24
#define RT_USING_SMP
#define RT_CPUS_NR 2
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 100
#define RT_USING_OVERFLOW_CHECK
#define RT_USING_HOOK
#define RT_HOOK_USING_FUNC_PTR
/* RT_USING_IDLE_HOOK — disabled to save stack/BSS */
#define IDLE_THREAD_STACK_SIZE 2048  /* CRITICAL: Increased from 512 to prevent Timer ISR stack overflow */
#define SYSTEM_THREAD_STACK_SIZE 2048  /* Increased from 512 for Timer ISR safety */
/* RT_USING_TIMER_SOFT — disabled to save code/BSS */

/* kservice options */

/* end of kservice options */
/* RT_KERNEL_IRQ_DBG — EVERY IRQ dumps early UART (floods console). Keep off. */
/* #define RT_KERNEL_IRQ_DBG */
/* Sync/SError: one-shot early ESR dump without kprintf (SMP debug). */
/* #define BSP_SMP_EXC_EARLY_DUMP */
/*
 * Early UART breadcrumbs (default OFF for production):
 *   BSP_BOOT_EARLY_MARK — primary: entry P2I0 / IBKSUE, pre_entry ESC…,
 *                         board BOOT/EIMR…, MMU 12345…
 *   BSP_SMP_EARLY_MARK  — secondary: a..e / I / wait .YN
 * Needs BSP_USING_HP232X_DEBUG_UART for ASM-side early_putc hardware.
 */
/* #define BSP_BOOT_EARLY_MARK */
/* #define BSP_SMP_EARLY_MARK */
/* #define RT_USING_DEBUG */  /* Temporarily disable for EL drop test */
/* #define RT_DEBUGING_ASSERT */
/* RT_DEBUGING_COLOR — disabled to save code */
/* RT_DEBUGING_CONTEXT — disabled to save code */

/* Inter-Thread communication */

#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT
/* RT_USING_MAILBOX — disabled to save code */
/* RT_USING_MESSAGEQUEUE — disabled to save code */
/* end of Inter-Thread communication */

/* Memory Management - Use small memory allocator for small heap */
/* #define RT_USING_SLAB */
/* #define RT_USING_MEMHEAP */
/* #define RT_MEMHEAP_FAST_MODE */
/* #define RT_USING_SLAB_AS_HEAP */
#define RT_USING_SMALL_MEM
#define RT_USING_SMALL_MEM_AS_HEAP  /* CRITICAL: Use small_mem as system heap */
/* RT_USING_MEMTRACE — disabled to save memory */
#define RT_USING_HEAP
#define RT_USING_HEAP_ISR       /* SMP: spinlock heap lock (he200); mutex hangs in finsh rt_calloc */
/* end of Memory Management */
#define RT_USING_DEVICE
#define RT_USING_DEVICE_OPS
#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE 128
#define RT_CONSOLE_DEVICE_NAME "uart0"
#define RT_USING_CONSOLE_OUTPUT_CTL
#define RT_USING_THREADSAFE_PRINTF
#define RT_VER_NUM 0x50300
#define RT_USING_STDC_ATOMIC
#define RT_BACKTRACE_LEVEL_MAX_NR 32
/* end of RT-Thread Kernel */

/* AArch64 Architecture Configuration — HP232X / KA200 with IRAM only */

#define ARCH_TEXT_OFFSET 0x0
#define ARCH_RAM_OFFSET 0x04000000
#define ARCH_SECONDARY_CPU_STACK_SIZE 4096
#define ARCH_HAVE_EFFICIENT_UNALIGNED_ACCESS
#define ARCH_HEAP_SIZE 0x0C000      /* 48KB heap */
#define ARCH_INIT_PAGE_SIZE 0x04000 /* 16KB page pool */
/* end of AArch64 Architecture Configuration */
#define ARCH_CPU_64BIT
#define RT_USING_CACHE
#define RT_USING_CPU_FFS
#define ARCH_MM_MMU
#define ARCH_ARM
#define ARCH_ARM_MMU
#define ARCH_ARMV8
#define ARCH_USING_ASID
#define ARCH_USING_IRQ_CTX_LIST

/* RT-Thread Components */

#define RT_USING_COMPONENTS_INIT
#define RT_USING_USER_MAIN
#define RT_MAIN_THREAD_STACK_SIZE 4096  /* Increased from 2048 to accommodate rt_components_init */
#define RT_MAIN_THREAD_PRIORITY 10

/* FINSH/Shell - msh console */
#define RT_USING_MSH
#define RT_USING_FINSH
#define FINSH_USING_MSH
#define FINSH_THREAD_NAME "tshell"
#define FINSH_THREAD_PRIORITY 20
#define FINSH_THREAD_STACK_SIZE 4096
/* #define FINSH_USING_HISTORY */
/* #define FINSH_HISTORY_LINES 5 */
#define FINSH_USING_SYMTAB
#define FINSH_CMD_SIZE 80
#define MSH_USING_BUILT_IN_COMMANDS
/* #define FINSH_USING_DESCRIPTION */
#define FINSH_ARG_MAX 10

/*
 * Serial flash update:
 *   flash update  → Zmodem (send_zmodem.py / sz) — needs RT_USING_ZMODEM
 *   flash updatey → Ymodem (send_ymodem.py)
 * Default: ZMODEM off (src under utilities/zmodem, not built).
 */
/* #define RT_USING_ZMODEM */
#define RT_USING_RYM

/* DFS — disabled; serial driver uses rt_device API directly, shell uses rt_device_read/write */

/* end of DFS: device virtual file system */

/* Device Drivers */

#define RT_USING_DEVICE_IPC
#define RT_UNAMED_PIPE_NUMBER 64
#define RT_USING_SERIAL
#define RT_USING_SERIAL_V1
/*
 * YMODEM SOH frame = 1+2+128+2 = 133 > 128 → old 128B RB overflows mid-packet
 * (host sees ACKs desync, board RYM_ERR_CODE -113 recv=0). Need ≥ STX frame.
 */
#define RT_SERIAL_RB_BUFSZ 2048
#define RT_USING_INTERRUPT_INFO
/* RT_USING_CLOCK_TIME — disabled to save code */
/* RT_USING_NULL — disabled to save code */
/* RT_USING_ZERO — disabled to save code */
/* RT_USING_RANDOM — disabled to save code */
/* end of Device Drivers */

/* C/C++ and POSIX layer */

/* ISO-ANSI C layer */

/* Timezone and Daylight Saving Time — disabled to save ctime (~4KB IRAM0) */

/* #define RT_LIBC_USING_LIGHT_TZ_DST */
/* #define RT_LIBC_TZ_DEFAULT_HOUR 8 */
/* #define RT_LIBC_TZ_DEFAULT_MIN 0 */
/* #define RT_LIBC_TZ_DEFAULT_SEC 0 */
/* end of Timezone and Daylight Saving Time */
/* end of ISO-ANSI C layer */

/* POSIX — minimal, no STDIO/FS/POLL/SELECT/TERMIOS to avoid DFS dependency */

#define RT_USING_POSIX_DELAY
/* RT_USING_POSIX_CLOCK — disabled to save code */
/* end of POSIX (Portable Operating System Interface) layer */
/* end of C/C++ and POSIX layer */

/* Network — not available on IRAM-only system */
/* end of Network */

/* Memory protection */

/* end of Memory protection */

/* Utilities */

/* RT_USING_ULOG — disabled to save code size */
/* end of Utilities */

/* ADT (Abstract Data Type) */

#define RT_USING_RESOURCE_ID
#define RT_USING_ADT
#define RT_USING_ADT_AVL
#define RT_USING_ADT_BITMAP
/* end of ADT */

/* Memory management */

/* RT_USING_MEMBLOCK removed - custom minimal MMU for 256KB IRAM1 */
/* Define minimal symbols for mm component compilation compatibility */
#define RT_PAGE_AFFINITY_BLOCK_SIZE 0x1000
#define RT_PAGE_MAX_ORDER 11

/* end of Memory management */

/* end of RT-Thread Components */

#define KA200_SOC

/* Hardware Drivers Config */

#define BSP_USING_UART
#define RT_USING_UART0
#define BSP_USING_GIC
/* #define BSP_USING_GICV2 */
#define BSP_USING_GICV3  /* Use GICv3 for KA200 SoC (GIC-500) */
/* #define RT_BSP_GIC_DBG */  /* GIC group config debug - disabled by default */
#define KERNEL_ASPACE_START 0x04000000
/* ARM generic timer as system tick (CNTPCT verified 31.25MHz on BL22). */
#define BSP_USING_CORETIMER
/* #define BSP_USING_APB_TIMER */
/* #define BSP_USING_APB_TIMER_AS_TICK */
#define HP232X_APB_TIMER_CLOCK 50000000
#define HP232X_APB_TIMER_TICK_ID 0
#define BSP_USING_SYSCTL_CLK
#define BSP_USING_HP232X_ARCH_TIMER_PROBE
/* #define RT_BSP_PMON_TEST */
/* #define BSP_USING_HP232X_PMON_BIND_CPU1 */
/* #define BSP_USING_HP232X_SMP_BIND_TEST */

/* end of Hardware Drivers Config */

#endif
