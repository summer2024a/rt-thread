# HP232x BSP 宏控制一览

> 源：`rtconfig.h`、`drivers/board.h`、`biz/biz_modules.h`。  
> 生产默认见 [BIZ_PORTING.md](../BIZ_PORTING.md) §3.2。  
> 更新日期：2026-07-16。

---

## 1. 总览：宏落在哪一层

| 层 | 宏前缀 / 机制 | 作用 |
|----|---------------|------|
| Boot / 镜像布局 | `BSP_USING_HP232X*` | BL21/BL22、early UART |
| 业务模块裁剪 | `BSP_DRV_MOD_*` → `BIZ_MOD_*` | SCons 是否编译对应 `.c`；源码 `#ifdef` |
| 启动自启 / 手启 | `BSP_*_DEFER` / `BSP_BIZ_SKIP_*` | boot 自动起线程 vs msh 步进 |
| Host 内存 / cache | `BSP_IRAM*_LOW_NC` → `BSP_BIZ_SKIP_HOST_DCACHE` | MMU NC 窗；热路径剔除 dcache |
| Flash 执行模型 | `BSP_FLASH_CPU0_WORKER` / `DIRECT` | SSI 擦写绑 CPU0 或调用核 |
| 日志 / 统计 | `BSP_BIZ_LOG_*` / `BSP_BIZ_PHASE_STATS` | 默认等级、时间戳、phase 累计 |
| 平台外设 | `BSP_USING_*` | UART/GIC/timer/sysctl |
| 诊断 early 标记 | `BSP_BOOT_EARLY_MARK` / `BSP_SMP_EARLY_MARK` | 冷启 UART 面包屑 |

---

## 2. Boot / 布局

| 宏 | 默认 | 说明 |
|----|------|------|
| `BSP_USING_HP232X` | 开 | 本板标识 |
| `BSP_USING_HP232X_UARTSETUP_BL22` | 开 | 内核在 IRAM0 **后** 256KB（`0x04040020`） |
| `BSP_USING_HP232X_BL21` | 关 | 与 BL22 互斥；内核在 IRAM0 **前** 半 |
| `BSP_USING_HP232X_DEBUG_UART` | 开 | early putc 硬件；配合 early mark |
| `BSP_HP232X_MM_MINIMAL` | 开 | 裁掉 mm_anon/mm_fault + MM shell；`mm_anon_stub` |

### klibc 打印 / 扫描（体积）

| 宏 | 默认（hp232x） | 说明 |
|----|----------------|------|
| `RT_KLIBC_USING_VSNPRINTF_STANDARD` | **关** | 关 → `rt_vsnprintf_tiny`；开 → std（含可选浮点） |
| `RT_KLIBC_USING_VSNPRINTF_LONGLONG` | **开** | tiny/std 均需：支持 `%lld`/`%llu`/`%llx` |
| `RT_KLIBC_USING_VSSCANF_NO_FLOAT` | **开** | 禁用 `%f` 扫描，不链 Newlib `strtod` |

浮点展示用 `utilities/hp_fmt.h`（`hp_fmt_pvt_*` / `hp_fmt_uuid_simple`），勿依赖 `%.3f`。


**注意**：`BSP_IRAM0_LOW_NC` 仅适合 **BL22**（前半 IRAM0 不作 RTT `.text`）。

---

## 3. 业务模块裁剪（`BSP_DRV_MOD_*`）

`biz/biz_modules.h` 把 `BSP_DRV_MOD_X` 映射为 `BIZ_MOD_X`；`biz/SConscript` / `drivers/SConscript` 按 `GetDepend` 决定是否编译。

| 宏 | 默认 | 编入内容 |
|----|------|----------|
| `BSP_DRV_MOD_EXEC_APU` | 开 | `biz_exec_apu.c`（DbgRead / APUDebug / PWM / ApuClock） |
| `BSP_DRV_MOD_EXEC_MISC` | 开 | `biz_exec_misc.c`（Store 等） |
| `BSP_DRV_MOD_EXEC_STRESS` | 开 | `biz_exec_stress.c` + msh `stress` |
| `BSP_DRV_MOD_EXEC_SELFTEST` | 开 | `biz_exec_selftest.c` + msh `self_test` |
| `BSP_DRV_MOD_FLASH_UPGRADE` | 开 | Host FlashWrite 路径（与 MISC 共同影响部分目标） |
| `BSP_DRV_MOD_FINSH` | 开 | `biz_finsh_cmds.c`（flash/biz/heart_beat/phase/…） |
| `BSP_DRV_MOD_EMMC_DLL` | **关** | `biz_emmc_dll.c` |
| `BSP_DRV_MOD_PCIE` | **关** | `drv_pcie.c`（关则不链入） |

增量联调：可只开一个 `BSP_DRV_MOD_*`，缩小镜像与故障面。

---

## 4. 启动：自启 vs 手启（DEFER / SKIP）

生产：**全部注释掉**（关 = boot 自启）。打开后对应 msh 子命令才编入。

| 宏 | 关（生产） | 开（隔离调试） |
|----|------------|----------------|
| `BSP_SMP_DEFER_SECONDARY` | `main`：leave-XIP → flush tables → release CPU1 | msh `smp start` |
| `BSP_FLASH_DEFER_INIT` | soft-init + `main` bringup + worker | msh `flash init` / `worker` |
| `BSP_BIZ_SKIP_THREADS` | `main` / worker 起 `emmc_biz` | msh `biz start [cpu]`（**不含** I2C） |
| `BSP_I2C_DEFER` | `INIT_ENV` 起 `i2c_mcu` / `mcu_err` | msh `i2c start` |

正交关系：

- I2C **只**由 `BSP_I2C_DEFER` 控制，与 `BSP_BIZ_SKIP_THREADS` 无关。  
- Flash worker 是否自启：看 `BSP_FLASH_DEFER_INIT`，与 emmc_biz 独立。

相关代码：`applications/main.c`、`biz/biz_subsys.c`、`biz/biz_finsh_cmds.c`。

---

## 5. SMP / Flash 执行 / emmc 绑定

| 宏 | 默认 | 说明 |
|----|------|------|
| `BSP_BIZ_EMMC_ON_CPU1` | 开 | `emmc_biz` 绑 CPU1 |
| `BSP_FLASH_CPU0_WORKER` | 开 | Flash 擦写 bounce 到 CPU0 worker |
| `BSP_FLASH_DIRECT_ON_CALLER` | **关** | 在调用核（常为 CPU1）直接跑；试验用 |
| `BSP_USING_HP232X_SPIN_TABLE` | **关** | 简易 spin-table；会破坏正常 IRQ，勿开 |
| `BSP_USING_HP232X_SMP_BIND_TEST` | **关** | SMP bind 试验钩子 |

---

## 6. Host IRAM 窗口与 dcache

| 宏 | 默认 | 说明 |
|----|------|------|
| `BSP_IRAM1_LOW_NC` | 开 | `0x100000000..0x10003FFFF` → Normal NC（Host Load scratch） |
| `BSP_IRAM0_LOW_NC` | 开 | `0x04000000..0x0403FFFF` → Normal NC（Host 描述符/数据，BL22） |
| `BSP_BIZ_SKIP_HOST_DCACHE` | **派生** | `board.h`：两 NC 宏**同时**开时自动定义 |

派生效果：`biz_emmc_biz` / `biz_emmc_exec` / `biz_exec_misc` 里 CRC / Store / `report_task_result` 的 `rt_hw_cpu_dcache_ops` **不编译**。关任一 NC 宏后恢复 dcache 维护。

其它 eMMC 相关：

| 宏 | 默认 | 说明 |
|----|------|------|
| `BSP_EMMC_DMA_CACHED_BSS` | 关 | 不用 `.dma_nocache`，改缓存 BSS + flush |
| `BSP_EMMC_HS400_100M` | **关（=200M，量产）** | 开=HS400@**100M** + `SDCLK_DC=0x3c`（对齐 hp640 `HS400_100M_CLOCK`）；**自动打开** `BSP_DRV_MOD_EMMC_DLL`；init 后 DLL 读扫 + HB 写探测（详见 `BIZ_PORTING.md` §4.9；**UART 已验**） |
| `BSP_EMMC_CUSTOM_DC` | 关 | 单独强制 HS400 `SDCLK_DC=0x3c`（100M 宏已隐含） |
| `BSP_DRV_EMMC_DEBUG` | 关 | eMMC 驱动详细日志 |

---

## 7. 日志与 phase 统计

| 宏 | 默认 | 说明 |
|----|------|------|
| `BSP_BIZ_LOG_BOOT_INFO` | 开 | 默认等级 **info**（否则 warn） |
| `BSP_BIZ_LOG_LOCATION` | 关 | 每条日志带 `func:line`（DEBUG 默认已带） |
| `BSP_BIZ_LOG_TIMESTAMP` | 关 | 每条带启动相对 `[sec.us]` |
| `BSP_BIZ_PHASE_STATS` | **关（生产）** | 开则 `msh phase` 可用，但全路径约 **-4% FPS**（已测 12492≈640） |
| `BSP_BIZ_HOTPATH_NO_TICK_IPI` | **关** | 已测：对 FPS 影响 &lt;1% |

运行时：`msh > log [warn|info|debug|N]`。msh 交互回显仍用 `rt_kprintf`。

---

## 8. 串口升级工具

| 宏 | 默认 | 说明 |
|----|------|------|
| `RT_USING_ZMODEM` | **关** | `utilities/zmodem`；`flash update` |
| `RT_USING_RYM` | 开 | Ymodem；`flash updatey`（`RT_SERIAL_RB_BUFSZ≥2048`） |

---

## 9. 平台外设 / tick

| 宏 | 默认 | 说明 |
|----|------|------|
| `BSP_USING_UART` / `RT_USING_UART0` | 开 | 控制台 |
| `BSP_USING_GICV3` | 开 | GIC-500（勿开 `GICV2`） |
| `BSP_USING_CORETIMER` | 开 | ARM generic timer 作 system tick |
| `BSP_USING_APB_TIMER` / `_AS_TICK` | 关 | APB timer 路径（历史/试验） |
| `BSP_USING_SYSCTL_CLK` | 开 | 时钟 lit 驱动 |
| `BSP_USING_HP232X_ARCH_TIMER_PROBE` | 开 | 启动探测 CNTPCT |
| `BSP_DRV_PVT_BOOT_SAMPLE` | 关 | 启动打印全部 PVT 通道 |

---

## 10. Early / 异常诊断（默认全关）

| 宏 | 说明 |
|----|------|
| `BSP_BOOT_EARLY_MARK` | 主核 early putc 面包屑 |
| `BSP_SMP_EARLY_MARK` | 从核 `a..e` / wait 标记 |
| `BSP_SMP_EXC_EARLY_DUMP` | Sync/SError 一次性 early ESR |
| `RT_KERNEL_IRQ_DBG` | 每个 IRQ dump（会刷屏） |
| `RT_BSP_GIC_DBG` / `RT_BSP_PMON_TEST` | GIC / pmon 试验 |

---

## 11. 推荐组合（速查）

**生产联调（当前）**

```c
#define BSP_USING_HP232X_UARTSETUP_BL22
#define BSP_BIZ_EMMC_ON_CPU1
#define BSP_FLASH_CPU0_WORKER
#define BSP_IRAM0_LOW_NC
#define BSP_IRAM1_LOW_NC          /* → BSP_BIZ_SKIP_HOST_DCACHE */
#define BSP_BIZ_PHASE_STATS
#define BSP_BIZ_LOG_BOOT_INFO
/* 全部 DEFER / SKIP 注释掉 */
/* RT_USING_ZMODEM 关；PCIE / EMMC_DLL 关 */
```

**冷启 / 单模块隔离**

```c
#define BSP_SMP_DEFER_SECONDARY
#define BSP_FLASH_DEFER_INIT
#define BSP_BIZ_SKIP_THREADS
#define BSP_I2C_DEFER
/* 再按需只留一个 BSP_DRV_MOD_* */
```

**Host WB 路径 A/B（恢复 dcache）**

```c
/* #define BSP_IRAM0_LOW_NC */
/* 或注释 BSP_IRAM1_LOW_NC — 任一即可去掉 SKIP_HOST_DCACHE */
```
