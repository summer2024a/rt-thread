# HP640 SPL 业务移植进展（hp232x RT-Thread）

> 本文档描述 `hp640_arm/common/spl/` 业务逻辑向 `lynxi-rtt/bsp/lynxi/hp232x` 的移植状态与测试方法，便于交接。  
> **上一阶段（已稳）**：eMMC **HS400@200M** + Host Flash 升级；Flash 冷启双核；SMP / emmc_biz / flash / I2C 自启；MCU I2C 代理；fpfifo CRC32 / `phase`。  
> **本阶段（UART 已验）**：**HS400@100M + DLL** 首发 HB 与 hp640 **同板对齐**（§4.9）；`rtconfig` 联调开 `BSP_EMMC_HS400_100M`，**量产请关回 200M**。  
> **并行待验**：Flash 冷启@100M；200M 回归护栏；READ_LOG / `mcu_err` e2e。
>
> 设计背景见 [doc/SPL_MIGRATION_DESIGN.md](doc/SPL_MIGRATION_DESIGN.md)；SMP 总入口 [doc/SMP_SETUP.md](doc/SMP_SETUP.md)；eMMC tuning 见 [doc/EMMC_TUNING.md](doc/EMMC_TUNING.md)；Flash 见 [doc/FLASH_PORTING.md](doc/FLASH_PORTING.md)；镜像/启动见 [doc/IMAGE_DESIGN.md](doc/IMAGE_DESIGN.md)；**宏控制**见 [doc/BSP_MACROS.md](doc/BSP_MACROS.md)；**代码段体积**见 [doc/CODE_SIZE.md](doc/CODE_SIZE.md)；BSP 见 [HANDOFF_SLIM.md](HANDOFF_SLIM.md)；**测试环境**见 [Test_ENV.md](Test_ENV.md)。

**更新日期**：2026-07-17（HS400@100M UART 闭环；同板 A/B vs hp640）

---

## 0. 交接一页纸

| 项 | 状态 |
|----|------|
| 测试板 | **192.168.58.36**（`lynxi` / `lx@123`），UART `/dev/ttyUSB0` @ 115200 |
| KA200 拓扑参数 | **`-l 0 -i 2 -k 30`** |
| eMMC HS400@**200M** + heartbeat | ✅ tap **0x34**；`emmc_biz` @ **CPU1**；**默认仅上电报一次**（`heart_beat_interval=0`） |
| eMMC HS400@**100M** + DLL + 首发 HB | ✅ **UART 闭环**（§4.9）：`DLL=0x32` + HB + Host ALIVE；联调开 `BSP_EMMC_HS400_100M`，**量产关回 200M** |
| Host 升级 Load + FlashWrite `@0xA6000` | ✅；默认 **CPU0 flash worker** + **`BSP_IRAM1_LOW_NC`** / **`BSP_IRAM0_LOW_NC`** |
| **Flash 冷启 → RTT 双核 + SSI/JEDEC** | ✅ leave-XIP 用 **flush**（非纯 inv）→ CPU1 ready → JEDEC → `emmc_biz entry CPU1` |
| **SMP / emmc_biz / flash 自启动** | ✅ 默认关 `BSP_*_DEFER` / `BSP_BIZ_SKIP_*`（见 §3.2） |
| **I2C MCU 从机 + `ka200 reg` 代理** | ✅ boot 自启；§4.5 |
| **Host 全路径压测（fpfifo_stress）** | ✅ 根因曾是 **bit-by-bit CRC32**；已换 **固化表 + 字批处理**（对齐 U-Boot `crc32.c`）；§4.8 |
| **biz 热路径 NC dcache** | ✅ 双 NC 宏时 **`BSP_BIZ_SKIP_HOST_DCACHE`**：CRC/Store/report **不编译** dcache 调用；§3.2 / §4.8 |

**拓扑说明（HP232x @ 58.36）**：32 颗 KA200 中 **chip30 = RTT 本 BSP**；其余为 **hp640 SPL**。MCU `i2c_scan` = 硬件在位检测。

**交给下一位的立即动作**：

1. 读 **§0 / §3.2 / §4.9 / §9**。  
2. **关 `BSP_EMMC_HS400_100M` 做 200M 回归**（量产默认仍是 200M）。  
3. 可选：Flash 冷启@100M（UART 已绿；冷启另验 leave-XIP + DLL）。  
4. 板测用 [Test_ENV.md](Test_ENV.md)；**勿破坏** `emmc_biz` 最高优先与 Host 升级回归。  
5. **禁止改 hp640 jumper**（`hp640_arm/.../spl.c` `CONFIG_HP640_JUMPER`）。  
6. 待验：READ_LOG / `biz_mcu_err_post` → Host 可见（§9）；DLL 失败路径曾触发 `i2c_mcu` `epc=0`。

---

## 1. 移植目标

将 hp640 SPL 中 **Host 可见的业务行为** 迁移到 RT-Thread，使 KA200 在 **不跑 U-Boot SPL** 时仍能通过 **eMMC + I2C** 与 Host MCU 交互：

| 业务能力 | hp640 入口 | hp232x 入口 | 板测 |
|----------|-----------|-------------|------|
| 上电 eMMC HS400 | `try_init_emmc()` | `drv_emmc_try_init(true)` | ✅ **200M**；✅ **100M UART** 见 §4.9 |
| 主动上报心跳 | `report_heart_beat()` | `biz_emmc_report_heartbeat()` | ✅ **200M** 仅上电首发；✅ **100M UART** |
| eMMC DLL offset | `sdhci_scan_dll_offset()` | `emmc_dll_offset_calibrate_100m()` + `biz_emmc_dll.c` | ✅ **100M UART**（DLL=`0x32`） |
| 轮询任务 + 执行 | `auto_run()` | `biz_emmc_biz_entry()` | ✅ 升级 + **fpfifo 全路径**（§4.8） |
| Host 升级 FlashWrite | `write_flash` @ `0xA6000` | `biz_emmc_exec` + `drv_flash` | ✅ |
| Flash 冷启 leave-XIP + JEDEC | jumper 后 open_flash | `main`：SMP 前 leave-XIP；再 `drv_flash_bringup` | ✅ 自启 |
| Flash 冷启双核 | — | flush leave-XIP + `mmu_flush_tables` + release | ✅ 自启 |
| **Host 读日志** | I2C `READ_LOG` | `drv_i2c.c` IRQ + 底半部 | 🟡 **下阶段验收** |
| **Host 报错** | GPIO78 + I2C | `drv_mcu_err_post()` → `mcu_err` | 🟡 **下阶段验收** |
| **MCU 代理读/写片内 MMIO** | — | `biz_i2c_proxy.c` + MCU `ka200 reg` | ✅ chip30 已验 |
| 其它任务 (Store/APU/…) | `spl_cmd.c` | `biz_exec_*.c` | 未系统回归 |

协议常量：`biz/biz_host_proto.h` / `biz/biz_config.c`（heartbeat `@0x400000`、query `@0x0500000`）。

### 1.1 移植范围说明

| 已对齐 | 未移植 / 有差异 |
|--------|----------------|
| eMMC init → HS400 → heartbeat → query/exec | U-Boot DM/MMC 层 |
| Host 升级 Load/FlashWrite（协议 + cache/NC） | — |
| Flash 冷启：strap win + leave-XIP（**flush+icache**）+ PA0 VA alias | jumper 仍不改；BL 链见 IMAGE_DESIGN |
| Flash SSI EPROMREAD/TO、CPU0 worker | 与 MCU Host 侧的 **I2C e2e 验收** |
| ADMA `.dma_nocache` @ `0x100040000` | — |

---

## 2. 架构对照

### 2.1 hp640 SPL（单线程轮询）

```
spl_cmd_run()
  → try_init_emmc(HS400)
  → auto_run()
       report_heart_beat()
       while (1) {
           query_task()      // eMMC @ 0x0500000
           exec_tasks()
           report_task_result()
       }
  // I2C：轮询/IRQ 与 MCU 从机协议（同进程）
```

### 2.2 hp232x RT-Thread（多线程 + SMP）— 生产自启

```
INIT_DEVICE  (CPU0)  biz_init: log/config/flash soft-init（无 SSI MMIO）/emmc soft
INIT_ENV     (CPU0)  biz_worker_init: apu, flash worker, i2c, i2c_mcu/mcu_err
components   (CPU0)  hp232x：不在此处放从核（须先 leave-XIP）
main()       (CPU0)
               1. leave-XIP(flush) + hp232x_mmu_flush_tables + secondary_up + wait
               2. drv_flash_bringup()   // JEDEC
               3. biz_emmc_biz_start()  // bind CPU1 + kick
emmc_biz     (CPU1)  emmc init → heartbeat → query/exec
i2c_mcu      (CPU0)  I2C0 IRQ63 底半部 + mailbox 代理
mcu_err      (CPU0)  GPIO78 → 准备 I2C 错误包
```

| 线程 | CPU | 优先级（越小越高） | 职责 | 源文件 |
|------|-----|-------------------|------|--------|
| `emmc_biz` | **1** | **2（biz 最高，勿改低）** | eMMC 主循环 | `biz/biz_emmc_biz.c` |
| `flash` | 0 | 5 | SSI 擦写 bounce | `drivers/drv_flash.c` |
| `i2c_mcu` | 0 | 3 | I2C 底半部 | `drivers/drv_i2c.c` |
| `mcu_err` | 0 | 4 | 错误上报 | `drivers/drv_gpio_mcu.c` |

I2C：**中断 + 信号量**；eMMC：**轮询**。跨核 / 冷启 SMP：[doc/SMP_SETUP.md](doc/SMP_SETUP.md)。

---

## 3. 模块移植清单

### 3.1 已完成（代码接入；板测见 §4）

| 模块 | hp640 源 | hp232x 实现 | 说明 |
|------|---------|-------------|------|
| 协议/配置 | `lynxi_hp640.h` | `biz_host_proto.h`, `biz_config.c` | Task/HeartBeat |
| 日志环缓冲 | `spl_log_buffer.c` | `biz/biz_log.c` | `@0x100050000`，供 I2C READ_LOG |
| eMMC | `spl_cmd.c` + `sdhci.c` | `drv_emmc_core.c` | HS400/ADMA3/tuning；**100M+DLL 见 §4.9** |
| eMMC DLL | `sdhci_scan_dll_offset()` | `drv_emmc_core.c`（100M）+ `biz_emmc_dll.c` | 读扫 `@0xFF400400`；HB 写探测 |
| eMMC 业务 | `auto_run()` | `biz_emmc_biz.c` | CPU1 + `hp232x_kick_cpu` |
| 任务执行 | `exec_tasks()` | `biz_emmc_exec.c` + `biz_crc32.c` | Load/FlashWrite；**CRC32 固化表**（§4.8） |
| Flash SSI | open/write_flash | `drv_flash.c` | **flush** leave-XIP；CPU0 worker；§4.7 |
| Host scratch | DDR_IRAM / IRAM0 低半 | MMU + `BSP_IRAM1_LOW_NC` / `BSP_IRAM0_LOW_NC` | 双开 → `BSP_BIZ_SKIP_HOST_DCACHE` |
| 相位统计 | SPL Round Stats / DDR 缓冲 | `BSP_BIZ_PHASE_STATS` + msh `phase` | **静默累计**，勿每包打印；§4.8 |
| SMP 从核 | spin-table | `board.c` + `entry_point.S` 门禁 | Flash 冷启 §4.3 / SMP_SETUP Part A |
| I2C/MCU GPIO | designware + GPIO78 | `drv_i2c.c` / `drv_gpio_mcu.c` | **默认自启**（§4.5） |
| I2C 代理 MMIO | — | `biz/biz_i2c_proxy.c` | `0xD0/0xD1` 绝对地址；对齐 32/16-bit 读 |
| DMA 区 | SPL BSS | `.dma_nocache` 64KB | **位置勿改** |

### 3.2 当前 `rtconfig.h` 要点（生产 / 联调默认）

```c
#define BSP_BIZ_EMMC_ON_CPU1             /* emmc_biz 独占 CPU1 */
#define BSP_BIZ_LOG_BOOT_INFO
/* #define BSP_BIZ_PHASE_STATS */         /* 开：msh phase；默认关（约 -4% FPS） */
#define BSP_DRV_MOD_EXEC_STRESS
#define BSP_DRV_MOD_FLASH_UPGRADE
#define BSP_DRV_MOD_FINSH
#define BSP_FLASH_CPU0_WORKER
/* #define BSP_FLASH_DIRECT_ON_CALLER */ /* 试验 OK；生产关 */
#define BSP_IRAM1_LOW_NC                 /* IRAM1 低 256KB Host scratch → NC */
#define BSP_IRAM0_LOW_NC                 /* IRAM0 低 256KB Host 描述符/数据 → NC */
/* 上两者同时定义时 board.h 自动 #define BSP_BIZ_SKIP_HOST_DCACHE
 * → CRC32 / Store / report_task_result 的 dcache 调用不编译进镜像 */

/*
 * HS400 时钟（勿与 200M 基线混淆）：
 * - 量产 / 已验路径：注释掉 BSP_EMMC_HS400_100M → HS400@200M（默认 DLL_OFFST=0x74）
 * - 联调 100M：打开下方宏 → 100M + SDCLK_DC=0x3c + 自动 BSP_DRV_MOD_EMMC_DLL
 *   对齐 hp640 HS400_100M_CLOCK（spl_cmd.c，默认常注释）
 */
/* #define BSP_EMMC_HS400_100M */         /* 量产关；100M 联调时打开（见 §4.9） */
/* #define BSP_EMMC_CUSTOM_DC */          /* 单独强制 DC=0x3c；100M 宏已隐含 */

/* —— 分步 / 规避宏 —— */
/* #define BSP_FLASH_DEFER_INIT */       /* 开：msh flash init|worker */
/* #define BSP_BIZ_SKIP_THREADS */       /* 开：msh biz start|upgrade（仅 emmc_biz） */
/* #define BSP_I2C_DEFER */              /* 开：msh i2c start；关=boot 自启 I2C（当前默认关） */
/* #define BSP_SMP_DEFER_SECONDARY */    /* 开：msh smp start */
/* #define BSP_BOOT_EARLY_MARK */        /* 主核 early putc */
/* #define BSP_SMP_EARLY_MARK */         /* 从核 early putc */
```

| 宏 | 关 | 开 |
|----|----|----|
| `BSP_SMP_DEFER_SECONDARY` | `main` leave-XIP+flush+放核 | msh `smp start` |
| `BSP_BIZ_SKIP_THREADS` | `main` 起 `emmc_biz@CPU1` | msh `biz start [cpu]`（**不含** I2C） |
| `BSP_I2C_DEFER` | `INIT_ENV` 起 i2c_mcu/mcu_err（**当前默认关**） | msh `i2c start`（调试用，§4.5） |
| `BSP_FLASH_DEFER_INIT` | flash bringup + worker 自启 | msh `flash init` / `worker` |
| `BSP_IRAM0_LOW_NC` + `BSP_IRAM1_LOW_NC` | Host 低窗 WB，biz 编译 dcache 维护 | 映 NC，并定义 **`BSP_BIZ_SKIP_HOST_DCACHE`** |
| `BSP_BIZ_PHASE_STATS` | 无累计（**生产默认**，FPS≈640） | 静默累计；msh `phase`；约 **-4% FPS** |
| `BSP_EMMC_HS400_100M` | HS400@**200M**（已验） | HS400@**100M** + DC=`0x3c` + DLL 校准（§4.9，**UART 已验**） |

**手启编译规则**：对应子命令仅在各自 DEFER/SKIP 宏打开时编入。  
**注意**：`BSP_BIZ_SKIP_THREADS` **不再**连同跳过 I2C；I2C 只由 `BSP_I2C_DEFER` 控制。

**日志统一**（仅 `bsp/lynxi/hp232x/`）：诊断用 `HP_LOGE/W/I/D`（或同后端的 `BIZ_*` / `LOG_*`），后端为 `biz_log` 环缓冲 + UART。  
格式：`[I] msg` / `[W] msg` / `[E] msg`；`[D] func:line msg`（DEBUG 才带位置）。`BSP_BIZ_LOG_LOCATION` 可强制每条都带 `func:line`。运行时等级：

```text
msh > log              # 显示当前等级
msh > log warn         # 或 error|info|debug|N（3/4/6/7）
```

`BSP_BIZ_LOG_BOOT_INFO` 开时默认 **info**。msh 交互（md/mw usage、flash 命令回显）仍用 `rt_kprintf`。

---

## 4. 当前板测状态（2026-07-16 @ 58.36；§4.9 为 2026-07-17）

### 4.1 hp640 SPL（A/B 基准）— ✅

Heartbeat + ID tap **0x35**（hp232x 目标 **0x34**）。

### 4.2 eMMC tuning — ✅

HS200 re-arm + iter≥63，tap **0x34**。见 [doc/EMMC_TUNING.md](doc/EMMC_TUNING.md)。

### 4.3 SMP + emmc_biz @ CPU1 — ✅（含 Ready 卡住 + Flash 冷启从核）

| 阶段 | 典型 log |
|------|----------|
| CPU1 | `[SMP] CPU1 ready` |
| Flash | `JEDEC=0xc22537`（或 leave-XIP ok） |
| 绑定 | `[biz] emmc_biz bound to CPU1` |
| 进入 | `[drv] emmc_biz entry CPU1` |
| 心跳 | `Heart-beat reported successfully` |

#### A）`emmc_biz` Ready 不跑（早期）— 已修

**曾复现**：`list_thread` 见 `bind=1 status=ready`，无 `entry CPU1`；`heart_beat`（CPU0）仍 OK。

**根因 / 修复**：idle 须 `rt_schedule()`+`WFE`；bind 后 `sgi_affinity_reset` + `hp232x_kick_cpu(1)`。见 `board.c` / `biz_subsys.c`。

#### B）Flash 冷启 CPU1 早期 EXC（2026-07-15）— 已修

UART 烧录双核正常；Flash jumper 冷启易在 `hp232x_mmu_secondary_init` 挂死（多个 `a`、无稳定 `c`）。

| 根因 | 修复 |
|------|------|
| leave-XIP 用 **`invalidate_dcache_all`** 丢掉脏页表/数据 | 改为 **`flush_dcache_all` + `invalidate_icache_all`**，再 B8→98 |
| 放核前页表未再刷 | release 前 **`hp232x_mmu_flush_tables()`** |
| 共享 mbox + SEV 唤醒多余核 | `entry_point.S`：`cpu_id ∈ [1, RT_CPUS_NR)`，其它核 WFE |
| 持 `_cpus_lock` 时 `kprintf` | 放核后 busy-wait；early 标记用 `BSP_SMP_EARLY_MARK` |

细节与 early 标记表：[doc/SMP_SETUP.md](doc/SMP_SETUP.md) **Part A**。

**心跳语义（勿混淆）**：

- `emmc_biz` 启动后 **自动发首发心跳**（与 `heart_beat_interval` 无关）；  
- `heart_beat_interval=0`（`biz_config_init` **默认**）→ **无周期心跳**；query 空转里 `process_heartbeat` 直接返回；  
- 周期心跳：Host Config 原语改 interval，或 msh `heart_beat` 手动报一次；  
- **200M**：成功日志多为 **`BIZ_DEBUG`**；**100M**：升为 **`BIZ_INFO`**（`Heart-beat reported ok`）便于联调；  
- msh `heart_beat` 与运行中的 `emmc_biz` **并发不安全**（§4.9）。

### 4.4 Host 在线升级 — ✅

| 步骤 | 状态 | 说明 |
|------|------|------|
| 拓扑门控 | ✅ | `[Link0]` + `[2] ALIVE` |
| `ka200_tools -u` | ✅ | Successfully |
| Load → `@0x100000000` | ✅ | |
| FlashWrite `@0xA6000` | ✅ | EPROMREAD RDSR；CPU0 worker |
| scratch NC / WB A/B | ✅ | 默认 NC |
| CPU1 DIRECT Flash | ✅ 试验 | 默认关 |

一键：`python3 scripts/flash_host_upgrade_test.py`。  
细节：[doc/FLASH_PORTING.md](doc/FLASH_PORTING.md)。

### 4.5 I2C — boot 自启 + MCU 代理（2026-07-16 交接）

#### 板型与职责

| 侧 | 芯片 | 固件 | I2C 角色 |
|----|------|------|----------|
| KA200 chip30 | RTT 本 BSP | `HP232x_KA200_Serdes_Update_20260716_v5.0.bin` | DW I2C0 从机 + **mailbox 代理**（`biz_i2c_proxy.c`） |
| KA200 其余 | hp640 SPL | 640 镜像 | 仅 **I2C 从机 init**（SAR `0x34+slot`），无 RTT 代理 |
| MCU | HP2320 | `HP232x_MCU_serdes_upgrade_20260716_V1.7.5.bin` | I2C2 master；CLI / FPGA UART 发命令 |

MCU **`i2c_scan`**：对 soc0–31 做 `HAL_I2C_IsDeviceReady`，**硬件在位**；同 mux 通道 8 颗全 ACK 为正常拓扑（非 bus fault）。  
**已移除** CLI 命令 `soc_rst`（自动复位仍由 `main` 里 `HandleSocResetSequence` 负责，勿在 CLI 手敲复位）。

#### KA200：自启 vs 手启（`BSP_I2C_DEFER`）

| 模式 | `rtconfig` | 行为 | msh |
|------|------------|------|-----|
| **自启（当前默认）** | 注释掉该宏 | `INIT_ENV` → `biz_i2c_ensure()` | （无 `i2c start`） |
| **手启（调试）** | `#define BSP_I2C_DEFER` | boot **不**起 I2C | `i2c start` / `i2c status` |

自启典型日志：

```text
[biz] worker: i2c
MCU I2C addr parsed: 0x3a (mux=6)
I2C MCU slave ready @ 0x3a IRQ63
i2c_mcu thread started
mcu_err thread started
[biz] worker: ready
```

#### 代理协议（Mode B，MCU ↔ KA200）

| 项 | 说明 |
|----|------|
| 邮箱 | I2C reg=`0x00`；帧头 4B + payload |
| 读 | cmd **`0xD0`**；payload `{ uint32_t reg_addr; uint8_t len; }`（**5B，绝对地址**） |
| 写 | cmd **`0xD1`**；payload 同上 + 数据 |
| KA 实现 | `addr = reg_addr` → `mmio_read_bytes` / `mmio_write_bytes`（**对齐优先 32/16-bit**，勿对 CPR 等 byte 读） |
| MCU CLI | `ka200 reg read <soc> <addr> <len>` / `write <soc> <addr> <byte>...` |
| mcu-tools | `-r ka200_reg -v soc,addr,len` / `-w ... -v soc,addr,hexbytes`（UART 包 **7B** 头） |
| 超时 | MCU `MCU_I2C_CMD_TIMEOUT=20` ms（对齐 HP640）；写后 `KA200_I2C_PROXY_DELAY_MS=2` |

**示例（chip30）**：

```text
ka200 reg read  30 0x12500064 4     # CPR BOOT_SELECT
ka200 reg write 30 0x04020000 0xA5 0x5A 0x12 0x34
ka200 reg read  30 0x04020000 4
```

#### 历史问题与修复（勿回退）

| 问题 | 修复 |
|------|------|
| MCU `i2c_scan` → KA msh 卡死 | KA：`drv_i2c.c` ISR 内 mask IRQ，BH 排空后再开；SDA stuck 限次 |
| 代理读 CPR → KA Data abort | `biz_i2c_proxy.c` 对齐 MMIO 访问（非 byte 读 `0x12500064`） |
| 早期 8/8 ACK 当假 ACK abort | **已撤销**；scan 扫满 32 槽（640 仅 I2C init 时 8/8 正常） |
| MCU 代理超时 4s 堵主循环 | 改为 **20ms**（`i2c_sensor.h`） |

#### 验收状态

| 项 | 状态 |
|----|------|
| I2C boot 自启 | ✅ |
| MCU `i2c_scan` + KA 存活 | ✅（58.36） |
| `ka200 reg` 读/写（chip30） | ✅ |
| **READ_LOG e2e** | ❓ 未验收 |
| **mcu_err → Host 可见** | ❓ 未验收 |

联调脚本：`scripts/mcu_cli_i2c_test.py`（先 KA 稳定 → 再 MCU `+++` → scan + reg R/W → `quit` + 30s + host reset）。  
MCU 协议详述：`lynxi-mcu/.../Doc/I2C_PROTOCOL.md`。

### 4.6 启动链诊断

| 串口最后输出 | 含义 | 常见原因 |
|-------------|------|----------|
| 仅 early `BOOT`（log&lt;4KB） | early 挂死 | IRAM1 BSS 溢出（§8） |
| `CPU1 ready` + `entry CPU1` + HB | 业务链 OK | — |
| `[biz] worker: ready` 后 KA msh 无回显 | I2C IRQ 风暴 / MCU scan 洪泛 | 确认 KA `drv_i2c` ISR mask 已合入；MCU 20ms 超时 |
| `ka200 reg read` → KA Data abort | CPR 等需 word 访问 | 用对齐地址 + len=4；或 IRAM scratch |
| `reg read fail` / I2C timeout | KA 未起 slave / 固件代际不一致 | 查 `I2C MCU slave ready`；KA+MCU 同刷 20260716 |
| Flash 冷启多个 `a`、无 `c`、EXC | leave-XIP 缓存 | 确认 flush 非 inv；开 `BSP_SMP_EARLY_MARK` |
| `emmc_biz … ready` 无 `entry` | CPU1 调度失效 | 缺 kick / idle 未 schedule（§4.3A） |
| `KA200>30:0.0` | chip30 无 RTT | 未起机或无心跳 |

### 4.7 Flash 冷启（BL jumper → RTT）— ✅ 自启（含双核）

**约束**：**不修改** hp640 jumper（`xip_memcpy` 后 jump，留下 `0x12600024=0xba…` XIP）。

#### 时机（生产）

| 阶段 | 做什么 |
|------|--------|
| `INIT_DEVICE` | **仅** flash soft-init（读 strap，不碰 SSI 写） |
| `main` 放核前 | `flash_xip_leave_open_flash()`：**flush_dcache_all** + inv icache → B8 → settle → 98 + AHB pinmux；再 `hp232x_mmu_flush_tables()` |
| `main` 放核后 | `drv_flash_bringup()`：绑窗 + 软读 CTRL0 门控 + JEDEC |
| 隔离调试 | 开 `BSP_FLASH_DEFER_INIT` / `BSP_SMP_DEFER_SECONDARY`，msh 步进 |

#### leave-XIP（2026-07-15 定稿）

| 项 | 做法 |
|----|------|
| cache | **`__asm_flush_dcache_all`**（clean+inv），**禁止**只 `invalidate_dcache_all` |
| 写序 | B8 → delay → 98 + pinmux；读回 `ssi_ctrl==0x98…` |
| 窗 | 读 strap，**不写** `BOOT_SELECT`；win1 → `0x07000000`→PA0 |
| JEDEC | 常见 **`0xc22537`**（MX25U643x） |

msh 步进（仅调试）：`flash open_flash` → `bind` → `peek`/`jedec`。

**验收 log（Flash 冷启自启）**：

```
[drv] flash soft-init (SSI deferred). BOOT_SELECT=0x10000010 win=1
…
[SMP] CPU1 ready (idle=0x…)
[drv] flash leave-XIP ok / JEDEC=0xc22537 …
[biz] emmc_biz bound to CPU1
[drv] emmc_biz entry CPU1
```

（首发 HB 成功现为 `BIZ_DEBUG`；默认 INFO 下以 `entry CPU1` + Host ALIVE 为准。）

### 4.8 Host 全路径压测与 CRC32（2026-07-16）— ✅ 根因已修

Host 应用源码：`/work/lynxlink/staging/fifo_test/fpfifo/fpfifo_stress.c`。

#### 命令与任务链

| 参数 | 含义 |
|------|------|
| `-d 0 -B 0:<chip>` | link0 + 单芯片（例 chip30=RTT） |
| `-r N` | 轮数 |
| `-b 64` | blkcnt=64 → input **32KB** |
| （无 `-n`） | 任务：**ExecBD → CRC32 → ExecBD** + Host compare |
| `-n` | 仅 ExecBD（无 CRC） |

```bash
./fpfifo_stress -d 0 -B 0:30 -r 10000 -b 64
```

#### 现象与根因（已修，勿回退）

| 项 | 说明 |
|----|------|
| Host FPS（修前） | RTT chip 远慢于同板 640 chip（约 **7×**，`-b 64`） |
| `-n`（无 CRC） | RTT ≈ 640 → 瓶颈不在 ExecBD/query |
| KA `avg_round`（修前） | RTT ~3800µs vs 640 ~560µs（同口径 window/`phase`） |
| **根因** | `biz_crc32_calc` 曾为 **逐 bit** 软件实现；32KB 多耗 ~3ms |
| **修复** | `biz/biz_crc32.c`：与 U-Boot `lib/crc32.c` 同 **固化 256 表 + LE 字批处理**（非 `DYNAMIC_CRC_TABLE`） |

#### `phase` 统计（勿再每 1 万包自动打印）

| 项 | 说明 |
|----|------|
| 宏 | `BSP_BIZ_PHASE_STATS`（`rtconfig.h`） |
| **生产默认** | **关** — 2026-07-17：开时 fpfifo ~11911 FPS，关后 **~12492**（≈ hp640 12430） |
| 行为 | **静默累计** query / round / ExecBD / CRC32 / other |
| msh | `phase` 打印平均；`phase reset` 清零（需先开宏重编） |
| 禁止 | 每 N 包 `rt_kprintf` 刷屏（干扰 Host 墙钟与 UART） |

示例：

```text
msh > phase
[phase] n=10000 stale_rehit=1
[phase] avg_query=40us avg_qt_rounds=4 avg_report=… avg_round=…
[phase] ExecBD  n=20000 avg=…us
[phase] CRC32   n=10000 avg=…us
```

#### NC 与 dcache（编译期）

`BSP_IRAM0_LOW_NC` **且** `BSP_IRAM1_LOW_NC` → `board.h` 定义 **`BSP_BIZ_SKIP_HOST_DCACHE`**：

```c
#ifndef BSP_BIZ_SKIP_HOST_DCACHE
    rt_hw_cpu_dcache_ops(...);   /* CRC32 / Store / report */
#endif
```

双 NC 默认开时，上述调用 **不编进镜像**。关任一 NC 宏后恢复编译 dcache 维护。

#### 交接验收建议

1. 刷含 CRC 表修复的 KA 镜像。  
2. Host：`./fpfifo_stress -d 0 -B 0:<rtt_chip> -r 10000 -b 64`，看 Avg FPS。  
3. KA：`phase`，确认 `CRC32 avg` 与 `avg_round` 合理（不再 ~3ms 量级空耗）。  
4. 回归：`-n` 仍应正常；Host 升级 + 冷启仍 PASS。

### 4.9 HS400@100M + DLL（2026-07-17）— ✅ UART 闭环 / 同板 A/B

> **背景**：部分板级 / FPGA 路径需 HS400 **100MHz**（hp640 `HS400_100M_CLOCK`）+ 固定 `SDCLK_DC=0x3c` + **`sdhci_scan_dll_offset`**。  
> **200M 已验路径勿改坏**：所有 100M 逻辑必须包在 `BSP_EMMC_HS400_100M` 下；量产默认 **关** 该宏。  
> **工作区现状**：`rtconfig.h` **打开** `BSP_EMMC_HS400_100M`（联调）；交付量产或回归 200M 前务必注释掉并重编。

#### 宏与代码入口

| 侧 | 开关 | 时钟 | DC | DLL |
|----|------|------|-----|-----|
| hp640 | `HS400_100M_CLOCK`（`spl_cmd.c`，常注释） | 100M | 常配 `CONFIG_HP640_CUSTOM_EMMC_DC`→`0x3c` | `sdhci_scan_dll_offset()`：读 `@0xFF400400`；**auto_run 内 scan 默认注释**，A/B 时临时打开 |
| RTT | `BSP_EMMC_HS400_100M`（`rtconfig.h`） | `EMMC_HS400_CLK_HZ=100M` | 强制 `SDCLK_DC=0x3c` | `emmc_dll_offset_calibrate_100m()` 在 `emmc_init_driver()` **HS400 OK 之后**；`biz_emmc_dll.c` 为 msh/`Exec` 补充 |

RTT 关键文件：`drivers/drv_emmc_core.c`（校准）、`biz/biz_emmc_dll.c`、`biz/biz_emmc_biz.c`（init→dll→HB）、`doc/BSP_MACROS.md`。

#### 同板 A/B 实测（chip30 UART @ 58.36，2026-07-17）

| 项 | hp640（临时开 `HS400_100M_CLOCK` + `CUSTOM_EMMC_DC` + auto_run DLL） | RTT（`BSP_EMMC_HS400_100M`） |
|----|------|------|
| 时钟 / DC | HS400 **100 MHz**，DC=`0x3c` | 同左 |
| DLL lock 初态 | `DLL has error status 0x3`（继续） | `dll: lock err stat=0x3`（continue） |
| Tuning | iter≈71，AT 路径过 | latch `tap=0x46` iter=63 |
| DLL scan | `final good DLL_OFFSET=**0x32**`，mlkdc=`0x7f` | read-eye `offset=**0x32**` mlkdc=`0x7f` range=0..100；HB write-probe OK |
| 首发 HB | `Heart-beat reported successfully` | `Heart-beat reported ok` + `Entering main task` |
| Host | ALIVE 位图含 chip30 | 同左（`lynx-showinfo` ALIVE=`FFFFFFFF`） |

> **注意**：hp640 A/B 后已 **关回** `HS400_100M_CLOCK` / `CUSTOM_EMMC_DC` / auto_run DLL（勿把 100M 留在 640 默认树）。RTT 联调宏仍开。

#### 已对齐（相对 hp640）

- HS400@100M + `SDCLK_DC=0x3c`（KA200M 日志可见）。  
- Tuning latch：`tap=0x46` / `iter=63` 可过（与 200M tap=0x34 不同，属预期）。  
- DLL 读扫范围 0..127；100M 取成功窗中点，**不做** `mlkdc/4` 回减。  
- flash 路径 / scan 完成路径的 `DLL_CTRL` 终值：flash 加载→`0x3`，scan 完成→`0x2`（与 hp640 `#ifdef HS400_100M_CLOCK` 分支一致）。  
- 扫测地址：**只读** `0xFF400400`（勿对该地址 ADMA 写）。  
- RTT 额外：**HB ARG 写探测**（确认 DATA 路径），再宣称 DLL OK；flash 候选 offset 写探测失败则 rescan。

#### 历史失败（已踩坑，勿回退）

| 现象 | 含义 / 处理 |
|------|----------|
| Tuning / HS400 OK，HB：`INT=0x1`（仅 CMD_COMPLETE） | 默认 `DLL_OFFST=0x74` 不适配 100M → 必须 scan |
| 对扫测地址 ADMA **写** | 总线打挂 → **只读**扫描；写探测用 HB ARG |
| flash 坏 offset（如 `0x2f`） | write-probe 失败后 rescan（勿盲信 flash） |
| msh `heart_beat` 与 biz 交错 | CPU0/CPU1 抢 eMMC；**勿用 msh HB 判首发** |
| DLL 失败 → `i2c_mcu` `epc=0` | `biz_mcu_err_post` 路径待顺手查 |

#### 成功日志（100M UART，勿输 msh `heart_beat`）

```text
[I] [drv] emmc: chip=KA200M HS400@100M SDCLK_DC=0x3c
[I] [drv] emmc: tuning OK tap=0x46 iter=63
[I] [drv] emmc: HS400 OK tap=0x46
[I] [drv] emmc: DLL scan … offset=0x32 … (HB write-probe OK)
[I] Heart-beat reported ok (index=… addr=0x8d400000)
[I] Entering main task processing loop
```

一键：`python3 scripts/remote_board_test.py biz`（需已开 `BSP_EMMC_HS400_100M`）。

#### 实现注意（已踩坑，勿回退）

| 点 | 说明 |
|----|------|
| 校准放在 **driver init** | 不依赖 SCons `GetDepend(BSP_DRV_MOD_EMMC_DLL)` 是否链上 `biz_emmc_dll.c` |
| 扫测地址只读 | 写探测用 **心跳 ARG**（`0x400000` + size 编码 → `0x8d400000` 类），与 `biz_emmc_report_heartbeat` 一致 |
| 失败后 `emmc_dll_recover_bus` | reset CMD/DATA + clear INT + wait idle，再扫下一 tap |
| 勿在 init 中途 `drv_flash_write` DLL | erase+program 含 **dcache-all**，易扰 ADMA；落盘留给 msh `emmc_dll` / biz 模块 |
| 200M 路径 | quiet / calibrate / HB INFO 均 `#ifdef BSP_EMMC_HS400_100M` |
| DLL lock `stat=0x3` | 100M 上 hp640/RTT **均有**；警告后继续，属预期 |

---

## 5. 关键文件地图

```
hp232x/biz/
├── biz_host_proto.h / biz_config.c   # heart_beat_interval 默认 0
├── biz_subsys.c              # kick_cpu + flash worker + i2c + emmc_biz start
├── biz_emmc_biz.c / biz_emmc_exec.c  # query/exec；Store/report dcache 受 SKIP 宏
├── biz_emmc_dll.c            # DLL msh/Exec；boot 与 driver 校准衔接
├── biz_crc32.c / biz_exec_misc.c     # CRC32 固化表；SKIP 宏下无 dcache
├── biz_log.c                 # I2C READ_LOG 数据源
├── biz_i2c_proxy.c           # 0xD0/0xD1 片内 MMIO 代理（绝对地址）
├── biz_finsh_cmds.c          # flash / biz / heart_beat / emmc_dll / phase
hp232x/drivers/
├── board.h                   # IRAM NC 窗口；BSP_BIZ_SKIP_HOST_DCACHE
├── drv_flash.c               # flush leave-XIP / bringup / worker
├── hp232x_mmu.c              # 0x07000000→PA0；mmu_flush_tables
├── board.c                   # idle schedule / kick / secondary_up
├── drv_i2c.c / drv_gpio_mcu.c
├── drv_emmc_core.c           # HS400；100M 时 emmc_dll_offset_calibrate_100m()
├── drv_emmc.h
hp232x/rtconfig.h             # BSP_EMMC_HS400_100M（联调开 / 量产关）
hp232x/utilities/
├── zmodem/                   # 串口 flash update（ZMODEM）；默认不编
                              # 需在 rtconfig.h 打开 RT_USING_ZMODEM
hp232x/applications/
├── main.c                    # SMP 自启 / flash bringup / emmc_biz
├── cmd.c                     # md/mw
hp232x/doc/
├── SMP_SETUP.md              # SMP 总入口（含 Flash 冷启 Part A）
├── FLASH_PORTING.md / IMAGE_DESIGN.md / EMMC_TUNING.md
libcpu/.../entry_point.S      # 从核 cpu_id 门禁
src/components.c              # hp232x 不在此放从核
```

hp640 对照：

- eMMC/升级：`common/spl/spl_cmd.c`
- Flash leave-XIP：`open_flash`（flush）+ ofdata `98`
- MCU I2C 从机 init：`spl_cmd.c` `mcu_i2c_init()`（640 KA200）
- **勿改** jumper：`common/spl/spl.c` `lynxi_640_read`

MCU（HP2320，与 KA I2C 代理配套）：

```
lynxi-mcu/Lynchip_mcu_HP2320/HP2320_APP/
├── Core/Src/cmd_ka200_i2c.c    # i2c_scan / ka200 reg CLI
├── Core/Src/ka200_i2c_cmd.c    # mailbox 组帧
├── Core/Src/mcu_to_ka200.c     # I2C2 + 自动 probe
├── Core/Inc/i2c_sensor.h       # MCU_I2C_CMD_TIMEOUT=20
└── Doc/I2C_PROTOCOL.md
lynxi-bsp-tools/lynxlink_tools/mcu_tools/   # FPGA UART ka200_reg
```

---

## 6. 编译与打包

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
export RTT_CC_PREFIX=/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
scons -j$(nproc)
# rtthread-header.bin          — UART / xmodem
# HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin  — Host 升级（scons PostAction）
# 当前联调：HP232x_KA200_Serdes_Update_20260716_v5.0.bin
```

MCU 编译与升级（58.36 示例）：

```bash
cd /work/lynxlink/lynxi-mcu/Lynchip_mcu_HP2320/build_app && ./build.sh
# HP232x_MCU_serdes_upgrade_20260716_V1.7.5.bin
lynx-showinfo -r -l 0 && sleep 5
mcu-tools -l 0 -i 2 -t 1 -u HP232x_MCU_serdes_upgrade_20260716_V1.7.5.bin
mcu-tools -l 0 -i 2 -t 1 reset_mcu
```

组包见 [doc/IMAGE_DESIGN.md](doc/IMAGE_DESIGN.md)。

---

## 7. 测试方法（速查）

完整环境：[Test_ENV.md](Test_ENV.md)。

```bash
python3 scripts/remote_board_test.py biz
python3 scripts/flash_host_upgrade_test.py
sudo python3 scripts/mcu_cli_i2c_test.py --skip-upgrade   # I2C 代理 e2e（需双 UART）
```

| 测试 | PASS |
|------|------|
| eMMC biz **200M** | `tuning OK tap=0x34` + `emmc_biz entry` + Host ALIVE / 首发 HB |
| eMMC biz **100M** | §4.9：`DLL … (HB write-probe OK)` + `Heart-beat reported ok` + Host ALIVE（**UART ✅**） |
| Host 升级 | `OK Load` + `OK FlashWrite` + `@0xa6000` 非 FF |
| Flash 冷启 | `CPU1 ready` + `JEDEC=0xc22537` + `emmc_biz entry CPU1` + 首发 HB |
| I2C 自启 | boot 日志 `I2C MCU slave ready @ 0x3a`（无需 `i2c start`） |
| I2C 代理 | MCU `ka200 reg read 30 0x12500064 4` → `ok:` + 4 字节；KA msh 仍活 |
| Host 全路径 | `fpfifo_stress -b 64` FPS 合理；`phase` 见 CRC32 avg 非 ms 级 |
| READ_LOG / mcu_err | Host 侧可读 log / 可见错误（**下阶段**；DLL 失败路径曾打挂 `i2c_mcu`） |

MSH（调试）：`phase` / `phase reset` / `heart_beat` / `emmc_dll` / `flash status` / `list_thread` / `log [level]`；开 DEFER 时 `smp start` / `biz start` / `flash init`。  
**注意**：`emmc_biz` 已跑时不要用 `heart_beat` 做 100M 判据（并发）。

---

## 8. 已解决的历史问题（勿回退）

| 问题 | 处理 |
|------|------|
| HS200 误锁 tap=0x0f | re-arm + iter≥63（EMMC_TUNING） |
| SMP 跨核 insert | `scheduler_mp.c` flush |
| Flash 全 `0xFF` / 假 WIP | RDSR 用 **EPROMREAD** |
| 间歇 verify `rd=0xff` | inv-all + 页 bounce；或 **`BSP_IRAM1_LOW_NC`** |
| stress 撑爆 IRAM1 | `STRESS_MAX_BLK=64` |
| query/心跳地址 | `0x0500000` / `0x400000` / `0x400208` |
| Flash 冷启 SSI SEA | flush+B8+98 + PA0 alias；**勿在 INIT_DEVICE JEDEC** |
| leave-XIP 纯 inv → CPU1 EXC | **flush_dcache_all** + flush tables 后再放核 |
| `emmc_biz` 一直 Ready | idle `rt_schedule` + `hp232x_kick_cpu(1)` |
| 放核后 CPU0 死锁 | 持 `_cpus_lock` 时勿 `kprintf`；busy-wait |
| components 过早放核 | hp232x 改由 `main` 在 leave-XIP 后放核 |
| I2C 代理 byte 读 CPR | `biz_i2c_proxy.c` 对齐 32/16-bit MMIO |
| MCU I2C 超时 4s | 改 20ms；KA 挂时快速失败回主循环 |
| Host 全路径 RTT ≫ 640（`-b 64`） | **bit CRC32** → `biz_crc32.c` 固化表+字批处理 |
| phase 每 1 万包打印 / DDR 脏计数 | 改为静默累计 + msh `phase`；640 侧勿用 BSS 累加器 |
| Host NC 区仍做 dcache | 双 NC 宏 → `BSP_BIZ_SKIP_HOST_DCACHE` 编译剔除 |

---

## 9. 待办（交接优先级）

### P0（已完成）— 核心业务 + I2C + 全路径 CRC（**HS400@200M**）

| 项 | 状态 |
|----|------|
| eMMC / Host 升级 / Flash 冷启双核 | ✅ §4.3–4.4、§4.7 |
| I2C boot 自启 + ISR 修复 | ✅ §4.5 |
| MCU↔KA `ka200 reg`（绝对地址，chip30） | ✅ §4.5 |
| Host fpfifo CRC32 慢路径修复 | ✅ §4.8 |
| KA+MCU **20260716** 固件对齐 | 交接时需板侧确认已刷 |

### P0（本阶段）— **HS400@100M ↔ hp640 A/B**（§4.9）

| 项 | 状态 |
|----|------|
| hp640 同板 UART 基准（临时开 100M+DLL） | ✅ `DLL_OFFSET=0x32` + HB；已关回默认 |
| RTT UART：`BSP_EMMC_HS400_100M` 首发 HB + Host ALIVE | ✅ §4.9 |
| 关 100M 宏后 **200M** 回归 | ⬜ 建议交付前再跑一次 |
| Flash 冷启@100M | ⬜ 未专门验收（UART 已绿） |
| flash `@0xe8029` 坏 offset / `@0xe7000` error flag 清理 | ⬜ 按需 |
| `biz_mcu_err_post(DLL_SCAN)` → `i2c_mcu` `epc=0` | ⬜ |
| READ_LOG / `mcu_err` Host 可见 | ⬜ |

### P1 — 冷启写回镜像 / 量产链

Host 升级写入 Flash 后 **冷复位**，确认不回 `.U`（头 + jumper `+0x20` 见 IMAGE_DESIGN）。  
量产 `rtconfig`：**注释** `BSP_EMMC_HS400_100M`（保持 200M）。

### P2

Store/APU/stress 全量；query 去 memcpy / 减 INT 重开等微优化；PCIe 按需；100M 量产化（DLL 落盘策略、去 INFO HB 等）。

---

## 10. 简要变更记录

| 日期 | 内容 |
|------|------|
| 2026-07-14 | Host 升级闭环；Flash NC/WB；冷启 JEDEC/`main` bringup；emmc_biz kick |
| 2026-07-15 | Flash 冷启 CPU1：leave-XIP **flush**（非 inv）+ `mmu_flush_tables`；entry 门禁 |
| 2026-07-15 | DEFER 三宏 + `smp`/`biz`/`flash` CLI；合并文档为 `SMP_SETUP.md` |
| 2026-07-15 | **生产默认**：关 SMP/BIZ/FLASH DEFER → 从核 + emmc_biz@CPU1 + flash 自启 |
| 2026-07-15 | **`BSP_I2C_DEFER`**：I2C 与 biz 正交；MCU 查地址后复位卡 I2C → 默认手启 `i2c start` |
| 2026-07-15 | IRAM0/1 低 256KB NC；biz 手启子命令按宏编入；本文交接更新 |
| 2026-07-15 | **Fix I2C hang**：`drv_i2c` ISR mask + SDA stuck 限次；MCU scan 假 ACK abort；任务 3 板测 PASS |
| 2026-07-16 | **I2C 默认 boot 自启**；`0xD0/0xD1` **绝对地址**（省 4B payload） |
| 2026-07-16 | **`biz_i2c_proxy.c`** 对齐 MMIO；CPR 代理读不再 Data abort |
| 2026-07-16 | MCU：scan 不再 8/8 abort；`MCU_I2C_CMD_TIMEOUT=20`；删 CLI `soc_rst` |
| 2026-07-16 | `mcu_cli_i2c_test.py` / mcu-tools 绝对地址；**I2C 代理板测 PASS** |
| 2026-07-16 | **CRC32** 固化表+字批处理；`phase` 静默统计；**`BSP_BIZ_SKIP_HOST_DCACHE`**；HB 默认仅上电一次 |
| 2026-07-17 | **`BSP_EMMC_HS400_100M`**：100M+DC`0x3c`+driver 内 DLL 校准；HB 写探测 |
| 2026-07-17 | **UART 闭环**：同板 A/B 与 hp640 同得 `DLL=0x32`；RTT 首发 HB + main loop；§4.9 更新 |