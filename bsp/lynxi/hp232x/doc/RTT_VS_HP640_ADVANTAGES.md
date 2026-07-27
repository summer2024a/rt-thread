# RTT（hp232x）相对 hp640 SPL 的优势分析

> 范围：KA200 上 **Host 可见业务**（eMMC 任务泵 / APU / Flash / I2C）在  
> `lynxi-rtt/bsp/lynxi/hp232x` 与 `hp640_arm/common/spl` 的对比。  
> 前提：640 业务主路径已落到 RTT **CPU1 `emmc_biz`**，协议与行为对齐（含 ADMA3 ExecBD）。  
> 日期：2026-07-25。  
> 相关：[BIZ_PORTING.md](../BIZ_PORTING.md)、[SMP_SETUP.md](SMP_SETUP.md)、[Biz_Performance_Optimization.md](Biz_Performance_Optimization.md)、[I2C_MCU_PROTOCOL.md](I2C_MCU_PROTOCOL.md)。

---

## 1. 一句话结论

RTT **不是**把 SPL 字节复刻进 RTOS，而是在 **协议对齐** 的前提下，用 **双核分工 + 模块化驱动 + 可运维开关** 换来：外设不堵业务核、联调可步进、MCU 代理/OTA 可扩展。  
**性能（fpfifo / ExecBD 热路径）与 hp640 基本持平**（IRAM1 WB + 固化 CRC 后）；优势在 **架构与工程化**，不在“跑得更快”。

---

## 2. 架构对比（优势来源）

| 维度 | hp640 SPL | RTT hp232x |
|------|-----------|------------|
| 运行模型 | 单线程 `auto_run` 忙等 | SMP：`emmc_biz@CPU1` + I2C/Flash@CPU0 |
| eMMC | 同进程轮询 ADMA3 | 同：CPU1 独占控制器（必要约束） |
| I2C / GPIO78 | 与业务同环交织 | 独立线程 + IRQ 底半部 |
| Flash | 同线程擦写 | CPU0 worker（可与 query 隔离） |
| 代码组织 | `spl_cmd.c` 巨石 | `biz_*` / `drv_*` 分模块 |
| 调试入口 | SPL CLI | msh + 宏 DEFER 步进 |
| 扩展 | 从机 I2C 为主 | 另有 MMIO 代理 + Phase B I2C OTA |

```text
hp640:  [ CPU ] ──── auto_run(query/exec) + I2C/Flash 交织 ────

RTT:    [ CPU1 ] ──── emmc_biz 忙等 eMMC（对齐 640 泵）────
        [ CPU0 ] ──── i2c_mcu / mcu_err / flash / msh ────
```

**RTT 优势**：业务泵仍占 CPU1 保持与 640 相当的热路径时延，同时 **MCU 扫描、Flash 升级、串口** 不再与 eMMC 死绑在同一执行上下文（640 单线程下任一长操作都会拖死整环）。

---

## 3. RTT 相对 640 的具体优势

### 3.1 双核隔离（最大架构优势）

- **CPU1**：只跑 `query → exec → report`，优先级最高，对齐 640 热路径时延。  
- **CPU0**：I2C 从机、错误脉冲、Flash bounce、Finsh。  
- 收益：Host `i2c_scan` / `ka200 reg` / OTA 轮询 STATUS 时，**不必停掉业务泵**（640 同核则互相饿死）。  
- 代价：跨核 Flash 需 cache 维护、prio 协调（已在 I2C OTA 路径补 flush + 临时提优先级）。

### 3.2 工程可维护性

| 点 | RTT |
|----|-----|
| 模块边界 | eMMC / APU / Flash / I2C / log / CRC 分文件 |
| 编译裁剪 | `BSP_DRV_MOD_*`、`BSP_BIZ_*`（见 [BSP_MACROS.md](BSP_MACROS.md)） |
| 联调 | `BSP_*_DEFER`：smp / biz / flash / i2c 可 msh 手启 |
| 文档 | BIZ_PORTING、EMMC_TUNING、FLASH、I2C、FPS 分册 |

640 的 `spl_cmd.c` 功能全，但改一处易牵全身；RTT 更适合多人交接与分阶段验收。

### 3.3 可观测与运行时控制

- 统一日志：`HP_*` / `BIZ_*` → 环缓冲 + UART；msh `log info|debug`。  
- Host `Config` 可改 `log_level`（对齐 640 指针绑定）。  
- 可选 `BSP_BIZ_PHASE_STATS`：静默累计 query/ExecBD/CRC，msh `phase`（生产默认关，避免 -4% FPS）。  
- 失败日志可打出 **真实失败 cmd/tag**（不仅是包头 Load）。  
- Write 路径有 DEBUG（imm/dst/size），便于 APU 包对照。

### 3.4 MCU 侧能力扩展（640 KA 侧无对等实现）

| 能力 | RTT | hp640 KA |
|------|-----|----------|
| I2C `0xD0/0xD1` 片内 MMIO 代理 | ✅ | 无（仅从机） |
| Phase B I2C OTA `0xE8..0xEB` | ✅ | 无 |
| SAR strap 对齐 | ✅（双采 + restore） | 有，实现不同 |

便于 MCU CLI / mcu-tools 不经 Host eMMC 升级路径操作单芯片。

#### 3.4.1 两个最有用的运维能力（失败场景）

相对 640，RTT 至少有两条**不依赖主线 eMMC 业务仍可用**的能力——这是选型时常被低估、但对现场排障/救砖很关键的点：

| # | 能力 | 何时有用 | 做法 |
|---|------|----------|------|
| **1** | **带外访问 KA200 寄存器** | 主线 eMMC 业务 **failed / hang / 卡死**（Host 任务泵不可用） | MCU / mcu-tools 经 I2C `0xD0/0xD1` **MMIO 代理**读写作片内寄存器；通路在 **CPU0 `i2c_mcu`**，与 CPU1 `emmc_biz` 解耦，业务泵挂了仍可查状态、改配置、对照 dump |
| **2** | **旁路 OTA 救砖** | **eMMC 初始化失败**（进不了业务环、无法走 Host 下发升级） | MCU 经 Phase B I2C OTA（`0xE8..0xEB`）把镜像写进 Flash；**不必依赖 eMMC 通路**，也避免再上复杂 **工装烧录** |

```text
正常：Host ──eMMC──► KA200 业务泵
失败带外：MCU ──I2C──► CPU0（MMIO 代理 / OTA）──► 寄存器或 Flash
              └─ 不经过 Host eMMC 主线 ─┘
```

640 侧同类场景往往只能串口/工装；RTT 用 **I2C 带外通道**把“业务挂了仍能看寄存器、init 挂了仍能刷固件”做成产品能力。协议细节见 [I2C_MCU_PROTOCOL.md](I2C_MCU_PROTOCOL.md)。
### 3.5 冷启与板级鲁棒性（RTT 显式处理）

- Flash jumper 冷启：**leave-XIP flush**（非纯 inv）+ `mmu_flush_tables` 再放核。  
- HS200→AT 前 **`reset(CMD\|DATA)`**，根治 RTT 侧 early@17（640 自身不误锁，但 RTT 路径必须修）。  
- sticky `@0xe7000`：先清再 HS400，避免 GPIO78 与 init 竞态（并规避曾出现的 `i2c_mcu` abort）。

这些是“能上板长期跑”的优势，不直接体现为 FPS。

### 3.6 性能：与 640 持平（非差异化卖点）

- 曾慢于 640：bit CRC、IRAM1 NC。  
- 现：**固化 CRC 表 + IRAM1 默认 WB** → fpfifo 全路径与 640 **基本相当**（见 [Biz_Performance_Optimization.md](Biz_Performance_Optimization.md)）。  
- `-n`（仅 ExecBD）两边持平 → eMMC/ADMA 控制面无系统性拖后腿。

→ **性能不作 RTT 相对 640 的优势项**；验收口径为“不慢于 640”。

### 3.7 ADMA3 / ExecBD（与 640 同特性，非 RTT 独有）

两边都用 ADMA3：传输侧用描述符承载 CMD/ARG/BLK + 数据，kick `ADMA_ID`，**每笔不必大量手写传输寄存器**。  
ExecBD：Host 预填 BD，CPU 只指向 `bd_addr`。  
→ 数据面模型一致；RTT 优势不在“多了一种 DMA”，而在 **把该模型嵌进 SMP/模块化框架**。

---

## 4. 公平对照与对齐项

| 点 | 说明 |
|----|------|
| 性能 | **两边基本持平**；不作为选型差异 |
| 640 单进程 | 无跨核 cache/prio，逻辑更直观；镜像更小 |
| eMMC 单主人 | 加核也难线性涨 Host 业务 FPS（瓶颈在 ADMA）；详见 **§5** |
| 环日志 freeze | error/warn 后若 wrap 则冻结（两边同类设计） |

### 4.1 READ_LOG / mcu_err：对齐 hp640 即可

RTT 已有环缓冲、`READ_LOG(0x01)`、`biz_mcu_err_post`→GPIO78 路径，语义应对标 640：

| 项 | hp640 | RTT 目标 |
|----|-------|----------|
| READ_LOG | I2C `0x01` 读日志环 | **同协议、同可见性**（对照同板 640 验收即可） |
| mcu_err | GPIO78 + I2C 错码；timeout 类不上报 | **同策略**（`STATE/CMD/DATA_TIMEOUT` 不报） |
| 去重 | 同码只报一次 | 已有 `s_triggered_mask` |

→ 不另开“RTT 独有运维模型”；**板测时与 hp640 对拍通过即结项**，无需额外产品指标。

---

## 5. 再添加核的效果分析（假想 Core2 / Core3）

> KA200 量产路径为 **双核**（CPU0 外设 + CPU1 eMMC）。本节讨论若 SoC 再提供 2 核，对 **当前 Host↔eMMC 业务泵** 是否有实质收益。

### 5.1 硬约束

- **SDHCI / eMMC 同时只能有一个软件主人**：`query` / `kick ADMA` / `wait DATA_END` / `report` 必须串行，**不能**双核并行敲控制器。  
- Host 任务包常带依赖（`Load → Write → ExecBD`），包内乱序并行不安全。  
- 因此：加核 **无法**把“一笔 ExecBD/Load 的硬件时间”摊薄。

### 5.2 若有 4 核，建议部署（非涨 FPS 优先）

| 核 | 职责 | 目的 |
|----|------|------|
| **CPU0** | I2C / mcu_err / Flash / msh | 现状，外设面 |
| **CPU1** | **唯一 eMMC 主人**（query / kick / wait / report） | 对齐 640 泵；不可拆 |
| **CPU2** | CRC32 / 大 Copy 等 **CPU 重活** worker | 仅当软件占比高时减墙钟 |
| **CPU3** | 长 Wait / DgbRead 轮询、统计等旁路（可选） | 隔离、可观测；对吞吐帮助小 |

```text
包: Load/ExecBD ──一律 CPU1──►
    CRC32      ──可投递 CPU2，report 前 join──►
```

### 5.3 对业务性能的效果

| 场景 | 加 Core2/3 是否实质提升 |
|------|-------------------------|
| 纯 ExecBD / Load 泵（`-n`、APU Load+ExecBD） | **基本无** — 时间在 ADMA |
| fpfifo 带 CRC（CRC 仍重时） | **有限** — CPU2 卸载可缩短 round；固化表+WB 后 CRC 已轻，收益更小 |
| Flash / I2C OTA 与业务并行 | **隔离收益** — 少抢占，不提高 biz FPS |
| 指望线性涨 Host 业务 FPS | **无** |

**结论**：再添加核对 **当前 eMMC 业务主路径没有实质性吞吐提升**；双核（0+1）已够。Core2 仅在确认存在稳定 CPU 瓶颈（如重 CRC）时值得做；Core3 多为隔离，不是第三倍性能。

### 5.4 每增加一核的静态成本（量级）

编译期 `RT_CPUS_NR+1` 时，大约：

| 项 | 增量 |
|----|------|
| `.text` | ≈0（SMP 代码已在） |
| BSS（`_cpus` + idle TCB/栈 + early 栈等） | **~7KB / 从核** |
| `ARCH_HEAP_SIZE` | 不自动增加 |

early 栈在从核首次调度进 idle 后逻辑上可闲置，但当前为静态 `.bss.noclean`，**无运行时回收**；为省 4KB 去复用不划算。

---

## 6. 优势适用场景（怎么用这张表）

| 目标 | 更合适 |
|------|--------|
| Host 压测 FPS / 业务行为对齐 | 两边相当；选谁看运维与扩展 |
| MCU 频繁 scan / reg / I2C OTA | **RTT** |
| eMMC 业务失败仍要读 KA200 寄存器 | **RTT**（§3.4.1 带外 MMIO） |
| eMMC init 失败仍要刷固件、免工装 | **RTT**（§3.4.1 旁路 OTA） |
| 多人维护、分阶段上板、DEFER 排障 | **RTT** |
| 最小镜像、无 RTOS | **640 SPL** |
| 指望再加核大幅提高 eMMC 业务 FPS | **不行**（§5） |

---

## 7. 小结表

| 类别 | 相对 640 |
|------|----------|
| 架构 | ✅ CPU1 业务泵 + CPU0 外设 |
| 工程 | ✅ 模块化、宏裁剪、msh/DEFER、文档 |
| 扩展 | ✅ I2C MMIO 代理、Phase B OTA |
| 运维救场 | ✅ 业务失败可带外读寄存器；eMMC init 失败可旁路 OTA（免工装） |
| 板级 | ✅ 冷启 leave-XIP、tuning early、sticky |
| 性能 | ＝ **持平**（非优势） |
| READ_LOG / mcu_err | ＝ **对齐 640 验收即可** |
| 再加核 | ✗ **对主业务 FPS 无实质提升**（eMMC 单主人） |

**总括**：RTT 的优势是 **在性能与协议对齐 640 的同时，做成可并存的双核 RTOS 产品形态**（含带外寄存器访问与旁路 OTA 救砖）；640 的优势是 **单线程简单、镜像小**。选型看运维/扩展，不看谁更快，也不靠堆核涨 eMMC 吞吐。
