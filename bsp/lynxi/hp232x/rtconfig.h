#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

/* HP232X BSP - two segment IRAM KA200 */
#define BSP_USING_HP232X

/* Enable UART debug for MMU initialization tracking */

/* Enable components init debugging */
/* Disable other debug outputs */
/* #define RT_USING_DEBUG */
/* #define RT_DEBUGING_ASSERT */

/* Use simple spin table for secondary CPUs */
#define BSP_USING_HP232X_SPIN_TABLE

/* RT-Thread Kernel */

/* klibc options — minimal, no float */
#define RT_KLIBC_USING_VSNPRINTF_LONGLONG
#define RT_KLIBC_USING_VSNPRINTF_STANDARD
#define RT_KLIBC_USING_VSNPRINTF_DECIMAL_SPECIFIERS
#define RT_KLIBC_USING_VSNPRINTF_INTEGER_BUFFER_SIZE 32
/* end of rt_vsnprintf options */

/* rt_vsscanf options */
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
#define RT_CPUS_NR 1
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 100
#define RT_USING_OVERFLOW_CHECK
#define RT_USING_HOOK
#define RT_HOOK_USING_FUNC_PTR
/* RT_USING_IDLE_HOOK — disabled to save stack/BSS */
#define IDLE_THREAD_STACK_SIZE 512
#define SYSTEM_THREAD_STACK_SIZE 512
/* RT_USING_TIMER_SOFT — disabled to save code/BSS */

/* kservice options */

/* end of kservice options */
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
/* RT_USING_HEAP_ISR — disabled: spinlock may not work before scheduler init */
#define RT_USING_HEAP
/* end of Memory Management */
#define RT_USING_DEVICE
#define RT_USING_DEVICE_OPS
/* RT_USING_INTERRUPT_INFO — disabled to save memory */
#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE 128
#define RT_CONSOLE_DEVICE_NAME "uart0"
#define RT_USING_CONSOLE_OUTPUT_CTL
#define RT_VER_NUM 0x50300
/* RT_USING_STDC_ATOMIC — disabled if toolchain builtins are unnecessary */
#define RT_BACKTRACE_LEVEL_MAX_NR 32
/* end of RT-Thread Kernel */

/* AArch64 Architecture Configuration — HP232X / KA200 with IRAM only */

#define ARCH_TEXT_OFFSET 0x0
#define ARCH_RAM_OFFSET 0x04000000
#define ARCH_SECONDARY_CPU_STACK_SIZE 768
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
#define RT_MAIN_THREAD_STACK_SIZE 2048  /* Increased from 1024 to prevent stack overflow with scheduling */
#define RT_MAIN_THREAD_PRIORITY 10  /* Higher priority to complete init before shell runs */
#define RT_USING_MSH
#define RT_USING_FINSH
#define FINSH_USING_MSH
#define FINSH_THREAD_NAME "tshell"
#define FINSH_THREAD_PRIORITY 20
#define FINSH_THREAD_STACK_SIZE 2048  /* Increased from 1KB to 2KB for input handling */
#define FINSH_USING_HISTORY
#define FINSH_HISTORY_LINES 3
#define FINSH_USING_SYMTAB
#define FINSH_CMD_SIZE 64
#define MSH_USING_BUILT_IN_COMMANDS
/* FINSH_USING_DESCRIPTION — disabled to save rodata */
#define FINSH_ARG_MAX 10
/* FINSH_USING_OPTION_COMPLETION — disabled to save memory */

/* DFS — disabled; serial driver uses rt_device API directly, shell uses rt_device_read/write */

/* end of DFS: device virtual file system */

/* Device Drivers */

#define RT_USING_DEVICE_IPC
#define RT_UNAMED_PIPE_NUMBER 64
#define RT_USING_SERIAL
#define RT_USING_SERIAL_V1
#define RT_SERIAL_RB_BUFSZ 128
/* RT_USING_CLOCK_TIME — disabled to save code */
/* RT_USING_NULL — disabled to save code */
/* RT_USING_ZERO — disabled to save code */
/* RT_USING_RANDOM — disabled to save code */
/* end of Device Drivers */

/* C/C++ and POSIX layer */

/* ISO-ANSI C layer */

/* Timezone and Daylight Saving Time */

#define RT_LIBC_USING_LIGHT_TZ_DST
#define RT_LIBC_TZ_DEFAULT_HOUR 8
#define RT_LIBC_TZ_DEFAULT_MIN 0
#define RT_LIBC_TZ_DEFAULT_SEC 0
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
#define KERNEL_ASPACE_START 0x04000000
#define BSP_USING_CORETIMER

/* end of Hardware Drivers Config */

#endif
