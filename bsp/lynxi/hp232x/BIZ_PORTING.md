# HP640 SPL 业务移植进展（hp232x RT-Thread）

> 本文档描述 `hp640_arm/common/spl/` 业务逻辑向 `lynxi-rtt/bsp/lynxi/hp232x` 的移植状态与测试方法，便于交接。  
> **本阶段移交重点**：eMMC + Host Flash 升级已稳；**Flash 冷启双核自启已通**；**SMP / emmc_biz / flash 默认 boot 自启**；**I2C 已拆自启/手启**（默认 **手启**，规避 MCU 查地址后复位卡 bus）。  
> **下一位接 P0**：I2C e2e（READ_LOG / mcu_err）+ 推动 MCU（`Lynchip_mcu_HP2320/build_app`）修复位时序后，再评估关 `BSP_I2C_DEFER`。
>
> 设计背景见 [doc/SPL_MIGRATION_DESIGN.md](doc/SPL_MIGRATION_DESIGN.md)；SMP 总入口 [doc/SMP_SETUP.md](doc/SMP_SETUP.md)；eMMC tuning 见 [doc/EMMC_TUNING.md](doc/EMMC_TUNING.md)；Flash 见 [doc/FLASH_PORTING.md](doc/FLASH_PORTING.md)；镜像/启动见 [doc/IMAGE_DESIGN.md](doc/IMAGE_DESIGN.md)；BSP 见 [HANDOFF_SLIM.md](HANDOFF_SLIM.md)；**测试环境**见 [Test_ENV.md](Test_ENV.md)。

**更新日期**：2026-07-15（I2C 自启/手启；MCU 复位卡 I2C 根因；交接）

---

## 0. 交接一页纸

| 项 | 状态 |
|----|------|
| 测试板 | **192.168.58.36**（`lynxi` / `lx@123`），UART `/dev/ttyUSB0` @ 115200 |
| KA200 拓扑参数 | **`-l 0 -i 2 -k 30`** |
| eMMC HS400 + heartbeat | ✅ tap **0x34**；`emmc_biz` @ **CPU1**（bind + `kick_cpu`） |
| Host 升级 Load + FlashWrite `@0xA6000` | ✅；默认 **CPU0 flash worker** + **`BSP_IRAM1_LOW_NC`** / **`BSP_IRAM0_LOW_NC`** |
| **Flash 冷启 → RTT 双核 + SSI/JEDEC** | ✅ leave-XIP 用 **flush**（非纯 inv）→ CPU1 ready → JEDEC → `emmc_biz entry CPU1` |
| **SMP / emmc_biz / flash 自启动** | ✅ 默认关 `BSP_*_DEFER` / `BSP_BIZ_SKIP_*`（见 §3.2） |
| **I2C MCU 从机** | 🟡 **默认手启**（`BSP_I2C_DEFER`）；根因见 §4.5；e2e READ_LOG/报错待验 |

**交给下一位的立即动作**：

1. 读 **§0 / §3.2 / §4.3 / §4.5 / §4.7** + [doc/SMP_SETUP.md](doc/SMP_SETUP.md) Part A。  
2. 板测用 [Test_ENV.md](Test_ENV.md)；**勿破坏** `emmc_biz` 最高优先与 Host 升级回归。  
3. 冷启验收：`[SMP] CPU1 ready` → `JEDEC=0xc22537` → `emmc_biz entry CPU1` + HB。  
4. **I2C**：上电后若未见 slave ready，msh 执行 `i2c start`（当前默认 DEFER）。与 MCU 联调务必先看 §4.5。  
5. **禁止改 hp640 jumper**（`hp640_arm/.../spl.c` `CONFIG_HP640_JUMPER`）。  
6. MCU 修复位时序后：注释 `BSP_I2C_DEFER` → I2C boot 自启；再验 READ_LOG / mcu_err e2e。

---

## 1. 移植目标

将 hp640 SPL 中 **Host 可见的业务行为** 迁移到 RT-Thread，使 KA200 在 **不跑 U-Boot SPL** 时仍能通过 **eMMC + I2C** 与 Host MCU 交互：

| 业务能力 | hp640 入口 | hp232x 入口 | 板测 |
|----------|-----------|-------------|------|
| 上电 eMMC HS400 | `try_init_emmc()` | `drv_emmc_try_init(true)` | ✅ |
| 主动上报心跳 | `report_heart_beat()` | `biz_emmc_report_heartbeat()` | ✅（自动首发；周期靠 Host config） |
| 轮询任务 + 执行 | `auto_run()` | `biz_emmc_biz_entry()` | ✅（升级路径） |
| Host 升级 FlashWrite | `write_flash` @ `0xA6000` | `biz_emmc_exec` + `drv_flash` | ✅ |
| Flash 冷启 leave-XIP + JEDEC | jumper 后 open_flash | `main`：SMP 前 leave-XIP；再 `drv_flash_bringup` | ✅ 自启 |
| Flash 冷启双核 | — | flush leave-XIP + `mmu_flush_tables` + release | ✅ 自启 |
| **Host 读日志** | I2C `READ_LOG` | `drv_i2c.c` IRQ + 底半部 | 🟡 **下阶段验收** |
| **Host 报错** | GPIO78 + I2C | `drv_mcu_err_post()` → `mcu_err` | 🟡 **下阶段验收** |
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
i2c_mcu      (CPU0)  I2C0 IRQ63 底半部 ← 下阶段重点
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
| eMMC | `spl_cmd.c` + `sdhci.c` | `drv_emmc_core.c` | HS400/ADMA3/tuning |
| eMMC 业务 | `auto_run()` | `biz_emmc_biz.c` | CPU1 + `hp232x_kick_cpu` |
| 任务执行 | `exec_tasks()` | `biz_emmc_exec.c` | Load/FlashWrite 已验 |
| Flash SSI | open/write_flash | `drv_flash.c` | **flush** leave-XIP；CPU0 worker；§4.7 |
| Host scratch | DDR_IRAM / IRAM0 低半 | MMU + `BSP_IRAM1_LOW_NC` / `BSP_IRAM0_LOW_NC` | 低 256KB 各映 NC |
| SMP 从核 | spin-table | `board.c` + `entry_point.S` 门禁 | Flash 冷启 §4.3 / SMP_SETUP Part A |
| I2C/MCU GPIO | designware + GPIO78 | `drv_i2c.c` / `drv_gpio_mcu.c` | **自启/手启**（默认手启，§4.5） |
| DMA 区 | SPL BSS | `.dma_nocache` 64KB | **位置勿改** |

### 3.2 当前 `rtconfig.h` 要点（生产 / 联调默认）

```c
#define BSP_BIZ_EMMC_ON_CPU1             /* emmc_biz 独占 CPU1 */
#define BSP_BIZ_LOG_BOOT_INFO
#define BSP_DRV_MOD_EXEC_STRESS
#define BSP_DRV_MOD_FLASH_UPGRADE
#define BSP_DRV_MOD_FINSH
#define BSP_FLASH_CPU0_WORKER
/* #define BSP_FLASH_DIRECT_ON_CALLER */ /* 试验 OK；生产关 */
#define BSP_IRAM1_LOW_NC                 /* IRAM1 低 256KB Host scratch → NC */
#define BSP_IRAM0_LOW_NC                 /* IRAM0 低 256KB Host 描述符/数据 → NC */

/* —— 分步 / 规避宏 —— */
/* #define BSP_FLASH_DEFER_INIT */       /* 开：msh flash init|worker */
/* #define BSP_BIZ_SKIP_THREADS */       /* 开：msh biz start|upgrade（仅 emmc_biz） */
#define BSP_I2C_DEFER                    /* 开：msh i2c start；关=boot 自启 I2C（当前默认开） */
/* #define BSP_SMP_DEFER_SECONDARY */    /* 开：msh smp start */
/* #define BSP_BOOT_EARLY_MARK */        /* 主核 early putc */
/* #define BSP_SMP_EARLY_MARK */         /* 从核 early putc */
```

| 宏 | 关 | 开 |
|----|----|----|
| `BSP_SMP_DEFER_SECONDARY` | `main` leave-XIP+flush+放核 | msh `smp start` |
| `BSP_BIZ_SKIP_THREADS` | `main` 起 `emmc_biz@CPU1` | msh `biz start [cpu]`（**不含** I2C） |
| `BSP_I2C_DEFER` | `INIT_ENV` 起 i2c_mcu/mcu_err | msh `i2c start`（**当前默认开**，§4.5） |
| `BSP_FLASH_DEFER_INIT` | flash bringup + worker 自启 | msh `flash init` / `worker` |

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

## 4. 当前板测状态（2026-07-15 @ 58.36）

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
- `heart_beat_interval=0`（默认）→ **无周期心跳**；周期需 Host config 或 msh `heart_beat`。

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

### 4.5 I2C — 自启 / 手启 + MCU 复位踩坑（交接必读）

#### 现象与根因（2026-07-15）

| 项 | 内容 |
|----|------|
| 现象 | boot **自启 I2C** 后可达 `[biz] worker: ready`，随后 I2C 卡死；**手启**则正常 |
| Host 侧 | `/work/lynxlink/lynxi-mcu/Lynchip_mcu_HP2320/build_app`：MCU **查完 KA200 I2C 地址后**再触发/伴随 KA200 复位 |
| 结论 | MCU 查询窗口与 KA200 **复位 + I2C slave 自启** 竞态；固件侧用 **I2C 手启** 规避，根治在 MCU 时序 |

#### 双模式（`BSP_I2C_DEFER`）

| 模式 | `rtconfig` | 行为 | msh |
|------|------------|------|-----|
| **手启（当前默认）** | `#define BSP_I2C_DEFER` | boot **不**起 `drv_i2c` / `i2c_mcu` / `mcu_err` | `i2c start` / `i2c status` |
| **自启** | 注释掉该宏 | `INIT_ENV` → `biz_i2c_ensure()` | （无 `i2c start` 子命令） |

实现：`biz/biz_subsys.c` 的 `biz_i2c_ensure()`（幂等）；与 `BSP_BIZ_SKIP_THREADS`（仅 emmc_biz）正交。

板测手启成功后典型日志：

```text
[main] i2c deferred — msh: i2c start
...
msh >i2c start
[biz] worker: i2c
MCU I2C addr parsed: 0x3a (mux=6)     # 58.36 实测常见
I2C MCU slave ready @ 0x3a IRQ63
i2c_mcu thread started
mcu_err thread started
```

| 项 | 现状 |
|----|------|
| 控制器 | DesignWare I2C0 `@0x10002000`，从机 |
| 中断 | IRQ **63** → 信号量 → `drv_i2c_bh_entry` |
| 协议 | `CMD_DATA=0x00`，`DATA=0x02`；`READ_LOG=0x01` |
| MCU 复位竞态 | 🟡 **已定位**；默认手启；待 `build_app` 修 |
| **Host↔KA200 READ_LOG e2e** | ❓ 未验收 |
| **mcu_err → Host 可见** | ❓ 未正式验收 |

### 4.6 启动链诊断

| 串口最后输出 | 含义 | 常见原因 |
|-------------|------|----------|
| 仅 early `BOOT`（log&lt;4KB） | early 挂死 | IRAM1 BSS 溢出（§8） |
| `CPU1 ready` + `entry CPU1` + HB | 业务链 OK | — |
| `[biz] worker: ready` 后全无 / I2C 死 | MCU 查地址 + 复位竞态 | 开 `BSP_I2C_DEFER`，msh `i2c start`（§4.5） |
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
Heart-beat reported successfully
```

---

## 5. 关键文件地图

```
hp232x/biz/
├── biz_host_proto.h / biz_config.c
├── biz_subsys.c              # kick_cpu + flash worker + i2c + emmc_biz start
├── biz_emmc_biz.c / biz_emmc_exec.c
├── biz_log.c                 # I2C READ_LOG 数据源
├── biz_finsh_cmds.c          # flash / biz / heart_beat msh
hp232x/drivers/
├── drv_flash.c               # flush leave-XIP / bringup / worker
├── hp232x_mmu.c              # 0x07000000→PA0；mmu_flush_tables
├── board.c                   # idle schedule / kick / secondary_up
├── drv_i2c.c / drv_gpio_mcu.c
├── drv_emmc_core.c
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
- **勿改** jumper：`common/spl/spl.c` `lynxi_640_read`

---

## 6. 编译与打包

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
export RTT_CC_PREFIX=/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
scons -j$(nproc)
# rtthread-header.bin          — UART / xmodem
# HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin  — Host 升级（scons PostAction）
```

组包见 [doc/IMAGE_DESIGN.md](doc/IMAGE_DESIGN.md)。

---

## 7. 测试方法（速查）

完整环境：[Test_ENV.md](Test_ENV.md)。

```bash
python3 scripts/remote_board_test.py biz
python3 scripts/flash_host_upgrade_test.py
```

| 测试 | PASS |
|------|------|
| eMMC biz | `tuning OK tap=0x34` + `Heart-beat reported` |
| Host 升级 | `OK Load` + `OK FlashWrite` + `@0xa6000` 非 FF |
| Flash 冷启 | `CPU1 ready` + `JEDEC=0xc22537` + `emmc_biz entry CPU1` + 首发 HB |
| I2C（下阶段） | Host 能 **READ_LOG**；错误路径可复现 |

MSH（调试）：`heart_beat` / `flash status` / `list_thread` / `log [level]`；开 DEFER 时 `smp start` / `biz start` / `flash init`。

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

---

## 9. 待办（交接优先级）

### P0（已完成）— eMMC / Host 升级 / Flash 冷启双核自启 / kick

见 §4.3、§4.4、§4.7。三宏默认关。勿改 jumper。

### P0（下一阶段）— I2C MCU 从机 e2e + MCU 复位时序

1. **MCU**：`Lynchip_mcu_HP2320/build_app` — 查 KA200 I2C 地址后勿与 KA200 复位抢窗口（§4.5）。  
2. KA200：默认手启 `i2c start`；MCU 修完后关 `BSP_I2C_DEFER` 做自启回归。  
3. READ_LOG / GPIO78 `biz_mcu_err_post` e2e。  
4. 改 I2C 后再跑 Host 升级 + Flash 冷启回归。

### P1 — 冷启写回镜像 / 量产链

Host 升级写入 Flash 后 **冷复位**，确认不回 `.U`（头 + jumper `+0x20` 见 IMAGE_DESIGN）。

### P2

Store/APU/stress 全量；PCIe/DLL 按需；bootwrapper 每核独立 mbox 槽（现为防御门禁）。

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
