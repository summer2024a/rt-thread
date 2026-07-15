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
| **Tuning** | 硬件 `TUNED_CLK` 置位即完成 | FPGA 模型 **iter≈17 误锁**，需 re-arm + 软件 latch（§2.4） |
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
| 完成判据 | 硬件置 `TUNED_CLK` | hp640：自然 **~70 iter, tap≈0x35**；hp232x RTT：**iter≈17 误锁 tap=0x0f** → 需软件干预 |
| 误锁处理 | 无（假定硬件正确） | **re-arm**：清 `TUNED_CLK`、置 `EXEC_TUNING`、reset CMD/DATA、重写 AT_CTRL |
| 手动结束 | 无 | **iter≥63 且 tap≥0x34** 时软件置 `TUNED_CLK`（latch） |
| 等待 DATA_INHIBIT | 通常等 CMD+DATA inhibit 清零 | **仅等 CMD_INHIBIT**（tuning 跳过 DATA_INHIBIT） |
| TIMEOUT_CONTROL | 数据/命令超时常设 | **tuning 路径不设**（hp640 `!data` 分支） |
| Tuning buffer | 可读 BUFFER 校验 pattern | **只等 DATA_AVAIL，不读 BUFFER** |
| AT_CTRL 写入 | 部分驱动在 set_ios 多处写 | **仅在 `emmc_exe_tuning()` 内写** |
| 命令收尾 | 因驱动而异 | **固定** `udelay(1000)` → 读清 INT → 失败再 reset(CMD\|DATA) |
| 调度环境 | 内核/UB 可能抢占 | RT-Thread 多线程；已验证关中断包裹 tuning ** alone 不能** 解决误锁 |

**结论**：对标准 eMMC，上述 re-arm / latch 属于 **非标准 workaround**；对本 FPGA 后端 + RTT 时序组合则为 **必需**。

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
| tuning re-arm + latch | 误锁 tap 导致 HS400 无 DATA_END |
| SDCLK_DC / AT / DLL 路径 | Synopsys PHY + efuse 板级参数 |
| 紧循环 reset poll | FPGA 模型时序 |
| ADMA3 + 固定 NC DMA 区 | 与 hp640 描述符/地址一致 |
| 轮询 INT | SPL 无中断 eMMC 路径 |
| Lynxi 固定地址协议 | Host 兼容性 |

---

## 3. A/B 对比数据（同板 192.168.58.36）

hp640 增加 tuning iter log 后与 hp232x 逐步对比：

| iter | hp640 AT_STAT | hp232x AT_STAT（修复前） |
|------|---------------|-------------------------|
| 0 | 0x7 | 0x7 |
| 16 | 0x17 | 0x17 |
| 17 | （继续 EXEC_TUNING） | **TUNED_CLK 误锁 tap=0x0f** |
| 31 | 0x26 | — |
| 63 | 0x46 | — |
| 70 | **完成，tap=0x35** | — |

前 17 步 trial tap 完全一致；**第 18 步起 hp232x 被硬件误判 tuning 完成**，而 hp640 继续扫描至 ~70 步。

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
2. **拒绝 premature lock**：iter=17 tap=0x0f 时 **re-arm**（clear `TUNED_CLK`、set `EXEC_TUNING`、reset CMD/DATA、重写 AT_CTRL），继续扫描。
3. **手动 latch**：re-arm 后 RTT 侧硬件仍可能跑满 128 iter 不自然置 `TUNED_CLK`；在 **iter≥63 且 tap≥0x34** 时软件置 `TUNED_CLK` 结束 tuning（hp640 自然完成约 iter=70 tap=0x35；0x34 同板验证 DATA 相 OK）。

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
[drv] emmc: tuning latch tap=0x34 iter=63
[drv] emmc: tuning OK tap=0x34 iter=63
[drv] emmc: HS400 OK tap=0x34
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

- 根因级：查明 iter=17 误锁的 MMIO/调度时序差异，去掉 re-arm + 手动 latch（恢复标准 tuning 语义）
- tap 精确对齐 hp640 0x35（当前 0x34 已稳定）
- CPU1 绑定 `emmc_biz`：SMP 跨核调度验证见 [SMP_SETUP.md](SMP_SETUP.md)；可用 msh `biz start 1` / 诊断线程
