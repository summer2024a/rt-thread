# HP232x eMMC HS400 / Tuning 问题分析与修复

> 板级：KA200M，FPGA 例化 eMMC 模型，DesignWare SDHCI（与 hp640 同 IP）  
> 对照基准：hp640 SPL `try_init_emmc()` + `sdhci_exe_tuning()`  
> 相关代码：`drivers/drv_emmc_core.c`，业务入口 `drivers/drv_emmc_biz.c`

**更新**：2026-07-13

---

## 1. 现象

| 阶段 | hp640 SPL | hp232x RT-Thread（修复前） |
|------|-----------|---------------------------|
| HS400 init | ✅ | ✅ |
| HS200 tuning 完成 iter | **~70** | **17（过早）** |
| AT_STAT tap | **0x35** | **0x0f** |
| heartbeat ADMA write INT | **0x3**（RESPONSE \| DATA_END） | **0x1**（仅 RESPONSE） |
| query/exec 主循环 | ⏸ 阻塞于 heartbeat |

寄存器 CMD/ADMA/CLK/SDCLK_DC/AT_CTRL 已与 hp640 对齐，**根因在 tuning 收敛**，不是 ADMA 描述符或 HS400 init 流程缺失。

---

## 2. 与标准 eMMC 通信协议的差别

本节说明 KA200/hp232x 路径相对 **JEDEC eMMC 标准 + SDHCI 通用驱动** 的差异。标准协议指：上电识别（CMD0/1/2/…）、EXT_CSD 切速（Legacy→HS→HS200→HS400）、HS200 发 CMD21 做 host sampling tuning、数据用 ADMA2/ADMA3 块读写。

本板 **不是** 把 eMMC 当普通存储盘挂载文件系统，而是 **Lynxi Host 协议** 在固定 LBA 地址上读写任务/心跳结构体；底层仍走 eMMC 块传输，但 init/tuning/PHY 行为因 **FPGA 例化后端** 与 **Synopsys DWC SDHCI 扩展** 与通用 Linux/U-Boot eMMC 驱动不同。

### 2.1 协议分层

| 层次 | 标准 eMMC / SDHCI | KA200 hp232x / hp640 |
|------|-------------------|------------------------|
| **应用** | 分区、FAT/ext4、GPT | Lynxi `HP640_Task` / `HeartBeat` @ 固定字节地址（见 §2.7） |
| **块传输** | READ/WRITE_MULTIPLE_BLOCK，ADMA2 常见 | **ADMA3** 描述符链（Synopsys 扩展），轮询 INT 非 GIC 中断 |
| **链路模式** | HS200 tuning → HS(DDR) → HS400 | 同 JEDEC 命令序列，但 **HS400_FPGA_MODE 初始化顺序不同**（§2.3） |
| **Tuning** | 硬件 `TUNED_CLK` 置位即完成 | FPGA 模型 **iter≈17 误锁**，RTT 需 early re-arm（§2.4）；**无 soft latch** |
| **PHY** | 板级 device tree / 固定 delay | **efuse 选 SDCLK_DC**、AT tap、vendor DLL（§2.5） |
| **后端** | 真实 NAND eMMC 颗粒 | **FPGA 例化 eMMC 模型**，时序窗口更窄 |

### 2.2 后端：FPGA 模型 vs 真实 eMMC

| 项 | 标准 eMMC | 本板 |
|----|-----------|------|
| 介质 | 物理 eMMC 芯片 | SoC/FPGA 内 **例化 eMMC 行为模型**（`EMMC_FPGA_BACKEND`） |
| 时序容差 | 量产颗粒 + 板级 SI 设计 | 模型对 **采样沿 / reset poll / tuning 迭代** 极敏感 |
| 验证方式 | 示波器 + 标准驱动 | 必须与 hp640 SPL **寄存器级 A/B** 对齐 |

hp640 通过 `HS400_FPGA_MODE`（`spl_cmd.c`）和 `CONFIG_HP640_CUSTOM_EMMC_DELAY`（紧循环 reset poll）专门适配该后端；hp232x 在 `drv_emmc_core.c` 中保留同等语义。

### 2.3 初始化顺序（HS400_FPGA_MODE）

**标准 U-Boot/Linux 常见顺序**：

```
Legacy 识别 → init_cmds → 逐步升速 → HS200 tuning → HS400
（identification 阶段 host 通常处于 Legacy/HS 时序）
```

**本板 hp640 / hp232x 顺序**（`try_init_emmc` / `emmc_try_init_hs400_host`）：

```
power / go_idle
  → **先** 设 host HS400 时钟 + UHS + timing（EMMC_CTRL / HOST_CONTROL2）
  → **再** init_cmds（CMD1/2/3/7/9/…）
  → emmc_set_emmc_mode：8bit → HS200 + tuning → HS DDR → HS400 card/host
  → ADMA3 + blocklen
```

| 差异点 | 标准 | 本板 |
|--------|------|------|
| HS400 host 时序时机 | 多在 card 切 HS400 **之后** | **init_cmds 之前** 即配 host HS400（FPGA 门控要求） |
| ENH_STROBE / EMMC_CTRL | HS400ES 场景 | hp640 在 pre-init 写 vendor `EMMC_CTRL` |
| CMD16 SET_BLOCKLEN | 常执行 | **HS400 DDR 下跳过**（对齐 hp640 `mmc_set_blocklen`） |

### 2.4 HS200 Auto-Tuning（CMD21）

标准 SDHCI **Execute Tuning**：host 反复发 `MMC_CMD_SEND_TUNING_BLOCK_HS200`，硬件扫 tap，**`HOST_CONTROL2.TUNED_CLK=1` 且 `EXEC_TUNING=0` 时认为成功**，通常数十次迭代内完成，驱动 **信任硬件结论**。

| 项 | 标准 SDHCI / 通用驱动 | 本板 hp640 / hp232x |
|----|----------------------|---------------------|
| 完成判据 | 硬件置 `TUNED_CLK` | **同左**（hp640 / RTT）；UART 所见 tap≈0x35 仅为该颗 chip 观测值 |
| 误锁处理 | 无（假定硬件正确） | RTT：**early re-arm**；若已 re-arm 则 **iter≥70 soft latch**（无绝对 tap 窗） |
| 手动结束 | 无 | 仅 **post-rearm** late latch；无 re-arm 时完全信硬件 |
| 等待 DATA_INHIBIT | 通常等 CMD+DATA inhibit 清零 | **仅等 CMD_INHIBIT**（tuning 跳过 DATA_INHIBIT） |
| TIMEOUT_CONTROL | 数据/命令超时常设 | **tuning 路径不设**（hp640 `!data` 分支） |
| Tuning buffer | 可读 BUFFER 校验 pattern | **只等 DATA_AVAIL，不读 BUFFER** |
| AT_CTRL 写入 | 部分驱动在 set_ios 多处写 | **仅在 `emmc_exe_tuning()` 内写** |
| 命令收尾 | 因驱动而异 | **固定** `udelay(1000)` → 读清 INT → 失败再 reset(CMD\|DATA) |
| 调度环境 | 内核/UB 可能抢占 | RT-Thread 多线程；已验证关中断包裹 tuning ** alone 不能** 解决误锁 |

**结论**：对标准 eMMC，early re-arm 属 **非标准 workaround**；对本 FPGA + RTT 早期误锁仍可能需要。终值 tap **随 chip 板位变化**，勿写死。

### 2.5 Host PHY / DLL（Synopsys DesignWare 扩展）

标准 eMMC 规范定义 bus speed mode，**不规定** tap delay 寄存器；以下为 **Synopsys SDHCI vendor 扩展** + Lynxi 板级配置：

| 寄存器 / 参数 | 标准 eMMC | 本板 |
|---------------|-----------|------|
| AT_CTRL / AT_STAT | 非 JEDEC，属 IP vendor | tuning 窗口与 **tap 值**（如 0x34/0x35）决定 DATA 相 |
| SDCLK_DC | 非 JEDEC | **efuse 区分芯片**：KA200M→`0x21`，KA200→`0x23`；可选 `BSP_EMMC_CUSTOM_DC=0x3c` |
| DLL lock | IP 相关 | init 前 `emmc_dll_config()`；pre-tuning err bit 仅 warn |
| Reset poll | 驱动自定义 | **100000 次紧循环**（hp640 `CONFIG_HP640_CUSTOM_EMMC_DELAY`），不用长 mdelay |

### 2.6 数据传输路径

| 项 | 标准 SDHCI 驱动 | 本板 |
|----|----------------|------|
| DMA 模式 | ADMA2 普遍；ADMA3 视 IP | **ADMA3** 描述符（hp640 `emmc_write_data` 路径） |
| 完成通知 | 常 IRQ + 底半部 | **轮询 `SDHCI_INT_STATUS`**（与 hp640 SPL 一致，无 eMMC GIC 依赖） |
| DMA 缓冲区 | 动态分配 / CMA | 固定 **`.dma_nocache @ 0x100040000`**（64KB，与 hp640 SPL BSS 同址） |
| Cache | 内核 DMA API 一致性 | 默认 **non-cacheable**；`BSP_EMMC_DMA_CACHED_BSS` 可选 cached+flush |
| INT_EN | 按驱动配置 | 对齐 hp640 **`SDHCI_INT_DATA_MASK`（含 bit22）** |

### 2.7 应用层：Lynxi Host 协议（非 JEDEC）

标准 eMMC 应用不会约定以下固定地址与结构；这是 **HP640 SPL 与 Host MCU/FPGA 之间的私有协议**（`drv_host_proto.h`）：

| 用途 | 地址 / 说明 | 标准 eMMC |
|------|-------------|-----------|
| 心跳上报 | `heart_beat_report_base_address` 默认 **0x400000** | 无；非 EXT_CSD |
| 任务查询 | `HP640_QUERY_TASK_ADDR` **0x0500000** | 无 |
| 任务包 | `HP640_Task` 16 条 × packed 结构 | 无 |
| 任务结果 | `task_result_report_base_address` 等 config | 无 |
| 传输方式 | ADMA3 **单块写** heartbeat / 读 task 区 | 等价于 raw LBA 读写，非 SCSI/MMC 上层命令 |

Host 侧通过 **读 eMMC 某 LBA 范围** 获取心跳与任务，与文件系统无关；hp232x 的 `drv_emmc_biz.c` 复刻 hp640 `auto_run()` 轮询逻辑。

### 2.8 小结：移植时需保留的非标准项

| 必须保留 | 不可照搬标准 Linux mmc 的原因 |
|----------|--------------------------------|
| HS400_FPGA_MODE 初始化顺序 | FPGA 后端 init_cmds 前门控 |
| tuning re-arm（仅 early） | RTT 早期误锁；**无** soft latch / 绝对 tap 窗 |
| SDCLK_DC / AT / DLL 路径 | Synopsys PHY + efuse 板级参数 |
| 紧循环 reset poll | FPGA 模型时序 |
| ADMA3 + 固定 NC DMA 区 | 与 hp640 描述符/地址一致 |
| 轮询 INT | SPL 无中断 eMMC 路径 |
| Lynxi 固定地址协议 | Host 兼容性 |

---

## 3. A/B 对比数据（同板 192.168.58.36 chip30，2026-07-22 复测）

同一 Flash 冷启、同参 sparse TRACE（`emmc tun: iter=`）：

| iter | hp640 HC2 / EXEC / TUNED / tap | RTT HC2 / EXEC / TUNED / tap |
|------|--------------------------------|------------------------------|
| 0–16 | `384b` / 1 / 0 / `0x07`…`0x17` | **完全相同** |
| **17** | `384b` / **1** / **0** / **`0x18`** | `388b` / **0** / **1** / **`0x0f`** ← early 误锁 |
| 18+ | 继续扫 | re-arm 后从 `tap=0x7` 重扫 |
| 成功 | **`TUNED=1 tap=0x35 iter=70`（纯 HW）** | soft latch 窗 `[0x30,0x3a]`（过渡） |

日志：Host1 `/tmp/ab_hp640_tun.log`、`/tmp/ab_rtt_tun.log`（及 `.csv`）。

**结论**：前 17 步 trial tap 一致；**仅 RTT 在 iter=17 被硬件误置 `TUNED_CLK`**。hp640 同座可扫到自然 `0x35@70`。根因在 RTT 相对 SPL 的 **CMD21 收尾/时序**（非 I2C/绑核/tick，见 §10），不是 AT_CTRL 初值（两边 `0xf1d0000`）。

hp640 TRACE：编译加 `-DLYNXI_EMMC_TUNING_TRACE`（默认关）。RTT：`BSP_EMMC_TUNING_TRACE`。

> 注意：tuning 期间勿长时间独占 UART（dense TRACE 会拖死打印）；Host ALIVE 可先 `lynx-showinfo -r` 且不开串口。

---

## 4. 根因分析

### 4.1 直接原因

RT-Thread 环境下，HS200 auto-tuning 在 **iter≈17** 时 `HOST_CONTROL2` 的 `TUNED_CLK` 被置位、`EXEC_TUNING` 清除，AT 锁到 **tap=0x0f**。该 tap 对 FPGA eMMC 模型的 HS400 DATA 相无效，ADMA 写 heartbeat 永远等不到 `DATA_END`。

### 4.2 为何 hp640 不会误锁

hp640 SPL 为 **单线程裸机轮询**，无 RT-Thread 调度、无 I2C 底半部线程；tuning 命令收尾时序与 hp640 `sdhci_send_command()` 完全一致时，硬件可持续扫描到 iter≈70。

已验证 **不是** 以下单独因素（逐项排除）：

| 尝试 | 结果 |
|------|------|
| 对齐 INT_EN / ADMA / CLK / SDCLK_DC / AT_CTRL | 仍 iter=17 |
| tuning 设 `TIMEOUT_CONTROL=0x0e` | **更差**（hp640 tuning 路径不设此寄存器） |
| buffer drain / 迭代间 delay | 无改善 |
| `rt_hw_interrupt_disable()` 包裹 tuning | 无改善 |
| AT_CTRL 多处写 vs 仅 exe_tuning 写 | 无改善 |

### 4.3 有效差异（修复点）

1. **tuning 命令收尾顺序** 对齐 hp640：成功/失败后均 `udelay(1000)` → 读清 INT → 失败再 `reset(CMD|DATA)`；**不设 TIMEOUT_CONTROL**。
2. **成功判据对齐 hp640**：`EXEC_TUNING=0` 且 `TUNED_CLK=1` 即成功；**无绝对 tap 上下限**（板位不同）。
3. **RTT early re-arm**：`iter<40` 且 `tap<0x20`（如 `0x0f@17`）→ re-arm 继续扫。
4. **RTT post-rearm latch**：re-arm 后硬件常不再置 `TUNED_CLK`（本次 `code=11`）；仅此时在 **iter≥70** 且仍 `EXEC_TUNING` 时 soft latch。  
   **无 re-arm 时不 latch**（避免扫窗中途 `0x4b` 强锁 → HB `INT=0x1`）。

### 4.4 环境因素（次要）

| 因素 | 说明 |
|------|------|
| FPGA eMMC 模型 | AT/tuning 时序敏感；hp640 用 `CONFIG_HP640_CUSTOM_EMMC_DELAY` 紧循环 reset poll |
| RT-Thread 多线程 | `i2c_mcu` 等线程在 init 阶段已启动；tuning 宜在 `emmc_biz` 内连续完成 |
| CPU1 绑定 emmc_biz | `BSP_BIZ_EMMC_ON_CPU1` 已启用；跨核流程见 [SMP_SETUP.md](SMP_SETUP.md) |

---

## 5. 修复后预期 log

默认开启 `BSP_DRV_LOG_BOOT_INFO` 时：

```
[drv] emmc: chip=KA200M HS400 SDCLK_DC=0x21
[drv] emmc: tuning early tap=0xf iter=17, re-arm
[drv] emmc: tuning latch(post-rearm) tap=0x?? iter=70   # 仅 re-arm 后；或硬件 TUNED_CLK
[drv] emmc: tuning OK tap=0x?? iter=??
[drv] emmc: HS400 OK tap=0x??
Heart-beat reported successfully (index=1)
Entering main task processing loop
```

判据：heartbeat 写成功、`Entering main task processing loop` 出现；失败时保留 `emmc_dump_regs("xfer_err")` 全量寄存器 dump。

---

## 6. 初始化流程（与 hp640 对齐）

```
drv_emmc_init()          # CPR 时钟、读 vendor offset（INIT_DEVICE）
    ↓
emmc_biz → drv_emmc_try_init(true) → emmc_init_driver()
    ↓
emmc_init()              # power → reinit/phy → legacy clk → go_idle
    ↓
emmc_try_init_hs400_host()   # HS400_FPGA_MODE：先设 host HS400 + init_cmds 前门控
    ↓
emmc_init_cmds()         # CMD1/2/3/7/9…
    ↓
emmc_set_emmc_mode()     # 8bit → emmc_select_hs400()
                              ├ HS200 + emmc_exe_tuning()  ★
                              ├ HS + DDR bus
                              └ HS400 host/card
    ↓
emmc_setup_adma3_host()  # ADMA3 + blocklen
    ↓
heartbeat / query / exec
```

相对标准 eMMC 的顺序差异见 **§2.3**；tuning 非标准行为见 **§2.4**。

---

## 7. 关键文件

| 文件 | 职责 |
|------|------|
| `drivers/drv_emmc_core.c` | SDHCI/ADMA3/HS400/tuning（`emmc_exe_tuning`） |
| `drivers/drv_emmc_biz.c` | auto_run：init → heartbeat → query/exec |
| `drivers/drv_host_proto.h` | Lynxi 应用层协议（§2.7） |
| `drivers/drv_subsys.c` | 创建 `emmc_biz` 线程 |
| `hp640_arm/drivers/mmc/sdhci.c` | A/B 对照（含 tuning iter log） |
| `hp640_arm/common/spl/spl_cmd.c` | `HS400_FPGA_MODE`、`try_init_emmc` |

---

## 8. 测试

```bash
# 编译
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x && scons -j$(nproc)

# 板测 192.168.58.36
grep -aE 'tuning|heart|xfer_ok|fail|tap=' /tmp/hp232x_run.log
```

开启寄存器 dump：在 `rtconfig.h` 定义 `BSP_DRV_EMMC_DEBUG`。

---

## 9. 后续可选

- **P0**：early@17 根因已修（HS200 后 `reset(CMD|DATA)` → HW `0x35@~70`）。调试脚手架（latch/re-arm/TRACE/A–X 宏）已删除，tuning 对齐 hp640 纯 HW 路径。
- 混板回归：各 chip tap 可不同；判据是 eMMC 可用 / Host ALIVE，不是 UART 的 0x35

---

## 10. Early 误锁根因追查（chip30）

### 10.1 测试拓扑（用户指定）

| 项 | 值 |
|----|-----|
| 主机 | **测试主机0 / Host1** `192.168.58.36` |
| 启动 | **Flash 冷启**（jumper） |
| chip30 串口 | `/dev/ttyUSB0` @ 115200 |
| chip31 串口 | `/dev/ttyUSB1` @ 115200 |
| 实验对象 | **仅 chip30** |
| 无 HB / eMMC 起不来 | **YMODEM** 救砖：`scripts/send_ymodem.py`（先 detach screen） |

```bash
# 开发机编译后 scp rtthread-header.bin → 58.36
# chip30 无 msh 仍活时：
sudo python3 send_ymodem.py --cmd 'flash updatey 0xa6000 0x40000' rtthread-header.bin
# 然后 lynx-showinfo -r -l 0 冷启，抓 USB0
```

### 10.2 已排除（§4.2 + §10）

INT_EN / ADMA / CLK / SDCLK_DC / AT_CTRL 对齐、TIMEOUT_CONTROL、buffer drain、`rt_hw_interrupt_disable` 包 tuning、AT_CTRL 写法 — **仍 early@17**。

| 实验 | 结果（chip30 Flash 冷启） | 日期 |
|------|---------------------------|------|
| **A** `BSP_I2C_DEFER`（tuning 前无 I2C） | 仍 `early tap=0xf iter=17`；见 `i2c deferred` | 2026-07-22 |
| **B** 关 `BSP_BIZ_EMMC_ON_CPU1`（emmc@CPU0） | 仍 `early tap=0xf@17`；re-arm 后扫满 128 → `code=11`（soft latch 因 AT 持续爬升未 stable×3） | 2026-07-22 |
| **D** `BSP_EMMC_TUNING_CMD21_GAP_MS=1`（循环内 mdelay） | 仍 `early 0xf@17`；`emmc@CPU1`；re-arm 后 latch 未成 → bring-up fail | 2026-07-22 |
| **E** `BSP_EMMC_TUNING_NO_TICK`（tuning 关本核 tick） | 仍 `early 0xf@17`；re-arm 后 TRACE/UART 夹杂 `\\0` 后停更（疑似 CPU1 卡死） | 2026-07-22 |
| **F** `AT_CTRL=0xf1d0004`（`SWIN_TH_EN`） | 仍 early@17；readback 确认写入 | 2026-07-22 |
| **G** `BSP_EMMC_TUNING_FAST_CMD21`（去 1ms quirk + 跳过 R1） | 仍 early@17；关 TRACE 后仍如此 → **非 CMD21 节拍** | 2026-07-22 |
| **H** `AT_CTRL=0x281d0004`（`SWIN_TH=0x28`+EN） | 仍 early@17 → **非 SWIN 阈值过小** | 2026-07-22 |
| **I** `BSP_EMMC_AT_CTRL_SKIP`（保持复位 `0xf000005`） | 仍 early@17 | 2026-07-22 |
| **J** re-arm **不** `reset(CMD\|DATA)` | 仍 early；仍需 window latch；本组出现 HB `INT=0x1` | 2026-07-22 |
| CMD21 顺序对齐 hp640（BLKSZ→MODE→DMA clear→CMD） | 仍 early@17 | 2026-07-22 |
| **K** pre-tune 快照 + `ATS(L/R/C)` | 见下 **§10.2.1** | 2026-07-22 |
| **L** re-arm 脉冲 `ATUO_TUNING_EN` | sticky 边沿可清（`ATS→0x6`）；**仍无 HW TUNED**，需 window latch | 2026-07-22 |
| **M** CMD21 `BUF_DRAIN` | 仍 early@17 `ATS=0x7160f` | 2026-07-22 |
| **N** 首扫 `AT_CTRL=0xf1d0001`（AT_EN） | 仍 early@17 | 2026-07-22 |
| **O** `BSP_EMMC_TUNING_MIN_CMD21`（无 R1/INT_ALL/quirk/mid-EXEC reset；SIGNAL=0） | 仍 `early 0xf@17`；`soft-fail` **从未触发** → **非** INT_ALL/reset 关窗；见 Host1 `/tmp/ab_o.log` | 2026-07-22 |
| **P** post-CMD21 FIFO pattern | FIFO **空**（`PRESENT=0x3ff00f0` 无 bit11） | 2026-07-22 |
| **P2** BRR 当下读 FIFO | 仍 `words=0`；AT 占数据通路，软件无法验 pattern；early 仍在；`/tmp/ab_p2.log` | 2026-07-22 |
| **Q** `TIMEOUT_CONTROL=0xe` 于 AT 扫描 | 仍 early@17（`TOUT=0xe` 已生效）；`/tmp/ab_q.log` | 2026-07-22 |
| **R** HS200 `set_timing` 后 `mdelay(20)` 再 tuning | 仍 early@17；`/tmp/ab_r.log` | 2026-07-22 |
| **S** `SW_TUNE_EN` 软件扫 `CENTER_PH_CODE` | **失败** `best_w=1`（几乎全相 CMD21 honor_ret 失败）；chip30 ALIVE bit30 掉，已 YMODEM 恢复 | 2026-07-22 |
| **T** 全静默：`SMP_DEFER`+`I2C_DEFER`+emmc@CPU0+`QUIESCE`(irq+tick+SIG=0) | **仍** `early 0xf@17 ATS=0x7160f`；QUIESCE enter/exit 均见；`/tmp/ab_t.log` → **否 SoC 噪声主因** | 2026-07-22 |
| **U** PHY/PAD/DLL/CPR pre-tune 对照 | PHY/PAD/DLL/CPR **全同**；仅 `TOUT` RTT=`0xa` vs hp640=`0xe`（§10 Q 已否 TOUT）；`/tmp/ab_u_rtt.log` `/tmp/ab_u_hp640.log` | 2026-07-22 |
| **V** CMD21：`BLKSZ/COUNT/MODE` **先于** inhibit wait（真对齐 hp640 `sdhci_send_tuning`） | **仍** `early 0xf@17`（`CMD21=BLKSZ_1st` 已生效）；`/tmp/ab_v.log` | 2026-07-23 |
| **W** CMD6 后 **CMD13** 等到 `RDY_FOR_DATA`（对齐 hp640 `__mmc_switch`；原为固定 `mdelay(10)`） | CMD13 **已生效**（`HS_TIMING=2 OK status=0x900 polls=1`）但 **仍** `early 0xf@17 ATS=0x7160f`；`/tmp/ab_w.log` → **非** CMD6 settle | 2026-07-23 |
| **X** CMD21 BRR 后等 `DATA_INHIBIT` 清再返回 | **仍** early@17；`di=0`（BRR 时已 idle）；`/tmp/ab_x.log` | 2026-07-23 |
| **Y** HS200 进 AT 前 PIO **CMD8 EXT_CSD** 回读 | `EXT_CSD PIO fail n=0 INT=0x18001`；随后 **无 early**，**HW `tap=0x35@71`**；`/tmp/ab_y.log` → 疑似 fail 路径 `reset(CMD\|DATA)` 副作用 | 2026-07-23 |
| **Y2** 仅 HS200 后 `reset(CMD\|DATA)`（无 CMD8） | **成功**：无 early，**HW `tap=0x35@70`**，无 latch；ALIVE=FFFFFFFF；`/tmp/ab_y2.log` | 2026-07-23 |

#### 10.2.1 §10 K 关键发现（为何总是 17）

同座、同参 sparse TRACE（`pre-tune` + `ATS=…(L/R/C)`），日志 Host1：`/tmp/ab_k_hp640.log`、`/tmp/ab_k_rtt.log`。

| 项 | hp640 | RTT |
|----|-------|-----|
| **pre-tune** CLK/HC/HC2/`EMMC_CTRL`/MSHC/MBIU/`AT_CTRL`/`SDCLK_DC`/SMPLDL/ATDL/`INT_EN`/`SIG` | **逐字段相同** | **相同** |
| iter16 | `EXEC=1 tap=0x17` L=0 R=0 | **相同** |
| **iter17** | `EXEC=1 TUNED=0 tap=0x18` L=0 R=0 | `EXEC=0 TUNED=1` **`ATS=0x07160f` → L=0x7 R=0x16 C=0xf** |
| 含义 | 仍在扫 trial | **AT 收口**：窗宽 `R−L=0x0f`（= `SWIN_TH_VAL`），中心 `0x0f` |

结论：
1. early@17 **不是** 进 tuning 前寄存器配错（pre-tune 已对齐）。
2. 总是 17：trial 从 `0x7` 走到 `0x16` 共 16 点，窗宽触达 `SWIN_TH=0x0f` 后 HW 置 `TUNED_CLK`。
3. **差在样本判定**：RTT 在 ≈`0x17` 处被 AT 判失败（封右沿 `R=0x16`）；hp640 同点继续 PASS（L/R 仍为 0）。软件侧两边 CMD21 均无 `INT_ERROR`。
4. re-arm 后 L/R 粘滞；**写 `AT_STAT=0` 无效**；**脉冲 `ATUO_TUNING_EN` 可清到 `ATS=0x6`**，但二次扫描仍不自然 `TUNED_CLK` → 仍靠 window latch。
5. FIFO drain / 首扫开 `AT_EN` **不能**去掉 early。

下一刀建议：early re-arm / window latch 可标为 **保底**（Y2 后正常路径不再触发）；混板回归后再考虑删除。

**根因（§10 Y/Y2，2026-07-23）**：HS200 进入后（CMD6→clk→`set_timing`）SDHCI **CMD/DATA FSM 粘滞**，导致 AT 在 trial≈`0x17` 误判 FAIL → 窗宽触达 `SWIN_TH` 后 early `TUNED`（`ATS=0x07160f`）。在 `emmc_exe_tuning` 前做 **`reset(CMD|DATA)`** 后，与 hp640 同座一致：**HW `tap=0x35@iter≈70`，无 early、无 latch**。

**结论（§10 A–Y2）**：early@17 根因已定位并修复（pre-AT CMD/DATA reset）。CMD13 / BLKSZ-first 等对齐项保留。Latch/re-arm 仅作保底。

**过渡 soft latch（2026-07-22）**：re-arm 后 AT 爬升时，优先在 tap∈`[0x30,0x3a]` 锁（chip30 验过：`latch tap=0x30` → `ALIVE` bit30 正常）；否则晚 floor 兜底。勿锁到 `0x4b` 一类高位。

### 10.3 A/B 矩阵（按序做，每次只改一项）

| # | 实验 | 做法 | 期望若为根因 |
|---|------|------|----------------|
| A | **I2C 未起再 tuning** | `rtconfig.h` 开 `BSP_I2C_DEFER`；冷启后勿 `i2c start`，看是否仍 `early tap` | 不再 early → I2C/IRQ 干扰 |
| B | **emmc_biz@CPU0** | 临时关 `BSP_BIZ_EMMC_ON_CPU1` | 不再 early → 跨核/从核时序 |
| C | **逐 iter TRACE** | 开 `BSP_EMMC_TUNING_TRACE`，UART0 抓 `emmc tun: iter=` 与 hp640 同 iter 的 HC2/tap 对照 | 定位首次 `TUNED=1` 的 iter |
| D | **拉长 CMD21 间隔** | tuning 循环内额外 `mdelay(1)`（对照 640 仅循环头 mdelay） | 若消失 → 轮询过密/总线恢复 |
| E | **tuning 前停 tick** | `emmc_exe_tuning` 内关 local timer（慎） | 若消失 → 调度/tick 抢占 |
| F–J | **AT_CTRL / FAST / rearm** | 见 §10.2 表 | 均已否 |

通过标准：**连续多次 Flash 冷启无 `tuning early`，且硬件自然 `TUNED_CLK`（无 post-rearm latch），HB/OTA 稳定**。

下一刀建议（未做）：对比 **HS200 进入前** 全量寄存器（CLK/PHY/`EMMC_CTRL`/`MSHC`）与 hp640；或在 hp640 TRACE 中打每 iter `INT_STATUS` 看是否存在“样本失败”使窗口重置。

### 10.4 宏（`rtconfig.h`，默认全关）

```c
/* #define BSP_I2C_DEFER */           /* A：tuning 前不起 I2C */
/* #define BSP_EMMC_TUNING_TRACE */   /* C：每 iter INFO 打 HC2/tap */
/* #undef  BSP_BIZ_EMMC_ON_CPU1 */    /* B：emmc_biz 绑 CPU0 */
/* #define BSP_EMMC_TUNING_FAST_CMD21 */
/* #define BSP_EMMC_AT_CTRL_SKIP */
/* #define BSP_EMMC_TUNING_REARM_NO_RESET */
```

当前 soft latch / HB 重试仅作 **能 OTA 的过渡**；§10 过关后应删掉 post-rearm latch，只保留（或也不要）early re-arm。
