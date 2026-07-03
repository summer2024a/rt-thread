# HP232X BSP Handoff 精简版

## 当前状态（2026-07-03）

HP232X BL22 启动链已能稳定进入 RT-Thread，系统 tick 已从 ARM Generic Timer 切换到 DesignWare APB Timer。

当前验证输出：

```text
ECO
POK!
IBKSUE
BOOT
IRAM1: bss@10005a000-10005ce70 page@10005d000-100061000 heap@100061000-100069000

- RT -     Thread Operating System
[GIC Monitor] Thread created
Hi, this is RT-Thread!
[GIC] #5 Tick=201
[GIC] #10 Tick=451
```

关键验证：

- `[GIC] #5` 在 `main()` 后约 2 秒出现；
- `[GIC] #10` 与 `#5` 间隔约 2.5 秒；
- `rt_thread_mdelay(500)` 正常唤醒；
- 系统 tick、线程超时、调度时间片已由 APB timer 正常驱动。

---

## 已完成任务

### 1. IRQ 后 PC 跳到 BSS 固定地址已修复

历史问题：第一次 Timer IRQ 后，PC/ELR 固定跳到 BSS 地址 `0x10005c07c`。

根因：

- HP232X `.bss` 位于 IRAM1 高地址，early 阶段跳过 BSS 清零；
- `rt_interrupt_enter_hook` 保留垃圾值；
- 第一次 IRQ 进入 `rt_interrupt_enter()` 后执行错误 hook，跳入 BSS。

修复：

- [board.c](drivers/board.c) 中在 C 阶段补清高地址 `.bss`；
- 修复 IRQ 上下文保存/恢复不对称问题；
- 修复线程切换时 SPSR/DAIF 保存逻辑；
- 增加 `RT_KERNEL_IRQ_DBG` 调试开关。

结果：

- 不再出现 `0x10005c07c`；
- 能启动到 RT-Thread banner、创建 pmon 线程、进入 `main()`。

---

### 2. ARM Generic Timer 频率不一致问题已定位

现象：

```text
CNTFRQ_EL0 = 31250000
CNTPCT_EL0 实测约 500000 Hz
```

表现为：

- `rt_thread_mdelay(500)` 被拉长约 62.5 倍；
- `[GIC]` 输出从预期约 2.5 秒一次变成约 156 秒一次。

已确认：

- bootwrapper 确实写入 `CNTFRQ_EL0=31250000`；
- bootwrapper 和 BL21 `pre_entry.S` 都包含 `0x08600000/08/0c/20` timer clock 配置；
- 在 RT-Thread BL22 入口重新写这些 write-only 寄存器也不会改变 CNTPCT 实际频率。

结论：

- HP232X BL22 当前路径下不能直接信任 `CNTFRQ_EL0` 作为 CNTPCT 真实频率；
- ARM Generic Timer 不再作为默认 system tick 源。

相关记录：

- [system_trim_plan.md](doc/system_trim_plan.md)
- memory: `hp232x-generic-timer-frequency-mismatch.md`

---

### 3. APB Timer 已移植为 system tick

新增驱动：

- [drv_apb_timer.c](drivers/drv_apb_timer.c)
- [drv_apb_timer.h](drivers/drv_apb_timer.h)

当前配置：

```c
/* #define BSP_USING_CORETIMER */
#define BSP_USING_APB_TIMER
#define BSP_USING_APB_TIMER_AS_TICK
#define HP232X_APB_TIMER_CLOCK 50000000
#define HP232X_APB_TIMER_TICK_ID 0
```

参数：

| 项目 | 值 |
|---|---|
| Timer IP | DesignWare APB Timer |
| Base | `0x10012000` |
| Channel | timer0 |
| IRQ | `66` = SPI34 = `32 + 34` |
| Clock | `50 MHz` |
| RT tick | `100 Hz` |
| Load count | `500000` |

初始化路径：

```c
#ifdef BSP_USING_APB_TIMER_AS_TICK
    rt_hw_apb_timer_init();
#elif defined(BSP_USING_CORETIMER)
    rt_hw_gtimer_init();
#endif
```

验证结果：

- APB timer IRQ 正常触发；
- ISR 中调用 `rt_tick_increase()`；
- `rt_thread_mdelay()` 正常；
- pmon 周期输出恢复到约 2.5 秒。

提交：

```text
05a8e786bf bsp: hp232x use APB timer for system tick
```

---

### 4. BL21 路径 ARM Timer 配置已补齐

[pre_entry.S](drivers/pre_entry.S) 中 BL21 路径已按 lynxi bootwrapper 顺序配置平台 generic timer clock：

```text
0x08600000 = 0x1
0x08600008 = 0xf
0x0860000c = 0xf
0x08600020 = 0x01dcd650
CNTFRQ_EL0 = 31250000
```

说明：

- 这是 BL21/pre_entry 路径配置；
- BL22 当前默认 system tick 不依赖 ARM Generic Timer。

提交：

```text
591ce80745 bsp: hp232x configure ARM timer in BL21 path
```

---

### 5. 系统裁剪计划已建立

新增文档：

- [system_trim_plan.md](doc/system_trim_plan.md)

已记录后续裁剪事项：

- APB timer 模式下从构建中移除 `gtimer.c`；
- 梳理 [drivers/tick.c](drivers/tick.c) 是否仍需保留；
- 清理旧 ARM timer 调试路径；
- 保持 pmon 输出轻量；
- 同步测试文档和调试文档。

---

## 当前关键文件

| 文件 | 作用 |
|---|---|
| [entry_point.S](../../../libcpu/aarch64/cortex-a/entry_point.S) | RT-Thread AArch64 入口，BL21/BL22 分支 |
| [pre_entry.S](drivers/pre_entry.S) | BL21 bootwrapper 风格 EL3→EL2→EL1 初始化 |
| [board.c](drivers/board.c) | MMU、BSS、GIC、UART、tick 初始化 |
| [drv_apb_timer.c](drivers/drv_apb_timer.c) | HP232X APB timer system tick 驱动 |
| [drv_uart.c](drivers/drv_uart.c) | UART0 console 驱动 |
| [lynxi.h](drivers/lynxi.h) | HP232X 外设基址、IRQ、内存约束 |
| [rtconfig.h](rtconfig.h) | 当前 BSP 配置 |
| [system_trim_plan.md](doc/system_trim_plan.md) | 系统裁剪后续计划 |

---

## 当前内存布局

```text
IRAM0 BL22 usable: 0x04040000-0x0407FFFF
  0x04040000-0x0404001F  PCIe boot header
  0x04040020-...         .head/.text/.rodata/.data
  ...                    .early_hp232x
  ...                    .mmu_table

IRAM1 usable: 0x100040000-0x10007FFFF
  0x100050000-0x10005ce70  .bss
  0x10005d000-0x100061000  page pool
  0x100061000-0x100069000  heap
  0x100069000-0x10007fff0  stack area
```

最近编译结果：

```text
text    data    bss     dec     hex
174220  32400   52848   259468  3f58c rtthread.elf
IRAM0 (File): 201.67 KB / 256 KB
IRAM1 (BSS):   51.61 KB / 256 KB
Total:        253.28 KB
```

---

## 标准测试流程

```bash
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build && scons -j$(nproc)
python3 remote_test.py
```

长时间观察 pmon：

```text
[GIC] #5 Tick=...
[GIC] #10 Tick=...
```

期望：

- 每 5 次 `rt_thread_mdelay(500)` 输出一次；
- 相邻 `[GIC]` 输出间隔约 2.5 秒；
- 无 `0x2323...`；
- 无 `0x100100000`；
- 无 IRQ 后跳 BSS 地址。

---

## 下一步任务

### P0. msh 启动

目标：恢复 RT-Thread shell/msh。

当前背景：

- APB timer tick 已稳定；
- `rt_thread_mdelay()` 已正常；
- UART0 console 输出正常；
- UART RX / 中断输入路径仍需验证。

建议顺序：

1. 重新开启最小 msh/finsh 配置；
2. 控制 shell 栈大小，避免 IRAM1 压力过大；
3. 先验证 shell 线程创建和 `msh >` 输出；
4. 再验证 UART RX 中断或 polling 输入；
5. 若 RX IRQ 不稳定，先用 polling console 方案恢复交互。

---

### P1. 多核启动

目标：恢复或实现 SMP/多核启动。

当前背景：

- 当前以单核运行；
- `RT_CPUS_NR = 1`；
- secondary CPU spin table / mailbox 相关代码历史上存在，但未作为当前主线启用。

建议顺序：

1. 梳理 bootwrapper spin-table 与 RT-Thread secondary entry 的关系；
2. 确认 mailbox 地址和 secondary release 地址；
3. 先只唤醒 CPU1，验证入口 marker；
4. 再接入 RT-Thread SMP 初始化；
5. 最后处理 per-CPU timer/interrupt/stack。

---

### P2. 系统裁剪

目标：进一步减少 IRAM0/IRAM1 占用，避免后续 msh/SMP 引入后超限。

优先事项见 [system_trim_plan.md](doc/system_trim_plan.md)：

1. APB timer 模式下从构建中移除 `gtimer.c`；
2. 梳理并裁剪 [drivers/tick.c](drivers/tick.c)；
3. 删除或归档无用 `.disabled` 调试文件；
4. 清理旧 ARM timer 诊断代码；
5. 将 pmon 保持为轻量周期健康检查；
6. 同步 [TEST_METHODOLOGY.md](TEST_METHODOLOGY.md) 和历史调试文档。

---

## 注意事项

1. 当前 BL22 默认 tick 源是 APB timer，不是 ARM Generic Timer。
2. `CNTFRQ_EL0=31250000` 与 `CNTPCT_EL0≈500kHz` 的 mismatch 已确认，避免再次把它误判为 GIC/调度问题。
3. `gtimer.c` 目前可能仍被构建系统编译进固件，但不参与当前 system tick 运行路径。
4. APB timer IRQ 为 66，因此 `MAX_HANDLERS` 已扩大到 128。
5. 后续启用更多外设/中断时，需继续检查 `MAX_HANDLERS` 和 IRAM1 BSS 压力。

---

**更新日期**: 2026-07-03  
**当前状态**: BL22 启动稳定，APB timer system tick 验证通过，下一步为 msh 启动、多核启动、系统裁剪。
