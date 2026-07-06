# HP232X 系统裁剪计划

## 目标

在 HP232X 仅 IRAM 运行约束下，持续移除未使用或重复的启动、调试、驱动路径，降低 IRAM0 代码体积和 IRAM1 BSS/栈压力，同时保持当前已验证的启动与系统 tick 行为。

## 内存基线（2026-07-06，SMP 双核 + list_isr）

BL22 模式，内核链接 IRAM0 后 256KB（`0x04040000–0x0407FFFF`），运行时数据在 IRAM1 后 256KB。

| 区域 | 占用 | 预算 | 余量 | 说明 |
|------|------|------|------|------|
| **IRAM0 (File)** | **209.74 KB** | 256 KB | **~46 KB** | `.head` + `.text` + `.data` |
| **IRAM1 (.bss)** | **93.61 KB** | 256 KB | **~162 KB** | noclean + 普通 BSS |
| 镜像 payload | 211 KB | — | — | `rtthread-header.bin` |

SMP 双核实板：`test_msh_shell.py all` PASS（help + list_isr）。

构建后复查：

```bash
cd bsp/lynxi/hp232x && scons -j8
python3 mkimage.py rtthread.bin rtthread-header.bin --dest-addr 0x04040000 --elf rtthread.elf
```

---

## 已完成裁剪

### ✅ P-mmu. 页表迁至 IRAM1 noclean（2026-07-06）

**背景**：SMP 双核后 IRAM0 占用 **233.74 KB（91%）**，仅剩 ~20 KB；`.mmu_table` 占 **24 KB** 且与 IRAM0 代码竞争。

**做法**：

- `hp232x_mmu_l1` … `hp232x_mmu_l3_ram1` 改为 `section(".bss.noclean.mmu_table")`，链接在 IRAM1 noclean 区（`link.lds`）
- MMU 关闭时 IRAM1 物理地址可访问（栈/BSS 已验证）；`hp232x_build_page_tables()` 在 `hp232x_mmu_init()` 内填表后再开 MMU
- SMP 从核仍共享同一套页表，`hp232x_mmu_secondary_init()` 中 dcache flush 不变

**结果**：

| 指标 | 迁移前 | 迁移后 |
|------|--------|--------|
| IRAM0 | 233.74 KB | **209.74 KB**（−24 KB） |
| IRAM1 BSS | 69.61 KB | **93.61 KB**（+24 KB） |
| 页表 VA | `0x04075000` | **`0x10005b000`** |

**验证**：192.168.49.81 实板 `test_msh_shell.py all` PASS。

**涉及文件**：[link.lds](../link.lds)、[hp232x_mmu.c](../drivers/hp232x_mmu.c)、[mkimage.py](../mkimage.py)

---

## 当前已确认事项

### APB Timer 已作为系统 tick 源

当前 HP232X 已从 ARM generic timer 切换为 DesignWare APB timer 作为 RT-Thread system tick：

- APB timer 基地址：`0x10012000`
- 默认通道：timer0
- IRQ：`66`，即 Linux DTS 中 `interrupts = <0 34 4>` 对应的 GIC SPI34
- 时钟：`50 MHz`
- tick 频率：`RT_TICK_PER_SECOND = 100`
- load count：`50000000 / 100 = 500000`

验证结果：

```text
Hi, this is RT-Thread!
[GIC] #5 Tick=201   ← main 后约 2.0s
[GIC] #10 Tick=451  ← 与 #5 间隔约 2.5s
```

说明 `rt_thread_mdelay()`、线程超时唤醒、时间片调度等系统 tick 路径已由 APB timer 正常驱动。

### ARM Generic Timer 当前不再作为系统 tick

当前配置意图：

```c
/* #define BSP_USING_CORETIMER */
#define BSP_USING_APB_TIMER
#define BSP_USING_APB_TIMER_AS_TICK
#define HP232X_APB_TIMER_CLOCK 50000000
#define HP232X_APB_TIMER_TICK_ID 0
```

[board.c](../drivers/board.c) 中根据配置选择 tick 源：

```c
#ifdef BSP_USING_APB_TIMER_AS_TICK
    rt_hw_apb_timer_init();
#elif defined(BSP_USING_CORETIMER)
    rt_hw_gtimer_init();
#endif
```

因此运行时不再调用 `rt_hw_gtimer_init()`。

## Plan 事项（待做）

### P0. IRAM0 `.text` 体积优化（当前主瓶颈）

IRAM0 余量 ~46 KB，后续功能扩展仍偏紧。优先查 `rtthread.map` 大符号：

- finsh 命令 / symtab 表
- 未使用的驱动或调试路径
- SMP 相关是否可 `#ifdef` 裁剪单核配置

目标：**IRAM0 ≤ 85%（≤218 KB）**，留 ≥38 KB 余量。

### P1. IRAM1 noclean 中 `early_page` 评估（~20 KB）

[entry_point.S](../../../libcpu/aarch64/cortex-a/entry_point.S) 中 `.bss.noclean.early_page`（`early_tbl*` + `early_page_array`）在 HP232X BL22 下**不走路** `init_mmu_early`，与 `hp232x_mmu_*` 重复。

- 计划：BL22 专用缩小或 `#ifdef` 排除，释放 IRAM1 noclean 空间（对 IRAM0 无直接收益，但减轻 IRAM1 布局压力）

### P2. APB timer 模式下从构建中移除 gtimer.c

**背景**：

虽然运行时不再使用 ARM generic timer，但 [libcpu/aarch64/common/SConscript](../../../libcpu/aarch64/common/SConscript) 默认通过：

```python
src = Glob('*.c') + Glob('*.cpp') + Glob('*.S')
```

把 [gtimer.c](../../../libcpu/aarch64/common/gtimer.c) 编译进固件。当前它只在以下条件下被移除：

```python
if GetDepend('RT_USING_PIC') == True:
    SrcRemove(src, ['gicv3.c', 'gic.c', 'gtimer.c', 'interrupt.c'])

if GetDepend('RT_CLOCK_TIME_ARM_ARCH') == True:
    SrcRemove(src, ['gtimer.c'])
```

**计划**：

增加 HP232X/APB tick 条件：

```python
if GetDepend('RT_CLOCK_TIME_ARM_ARCH') == True or GetDepend('BSP_USING_APB_TIMER_AS_TICK') == True:
    SrcRemove(src, ['gtimer.c'])
```

**预期收益**：

- APB timer tick 模式下不再编译未使用的 ARM generic timer 代码；
- 避免后续误以为 `gtimer.c` 仍参与系统 tick；
- 减少 IRAM0 代码体积。

### P3. 清理旧 ARM timer 调试/测试路径

**背景**：

历史上为了排查 CNTP timer、GIC PPI30、`CNTFRQ_EL0` 与 `CNTPCT_EL0` 频率不一致问题，代码中保留过多处临时测试逻辑。

**计划**：

- APB timer tick 模式下，保留 `#ifdef BSP_USING_CORETIMER` 包裹的 ARM timer 测试代码，但默认不编译；
- 确认不再需要后，可进一步删除或移入调试文档；
- 保留必要结论：HP232X BL22 下 `CNTFRQ_EL0 = 31.25 MHz` 但 `CNTPCT_EL0` 实测约 `500 kHz`，不适合作为当前 system tick 默认源。

### P4. 梳理 tick.c 是否仍需保留

**背景**：

[drivers/tick.c](../drivers/tick.c) 也是 ARMv8 generic timer 风格的 tick 实现，但当前 board 初始化路径使用的是：

- APB timer：`rt_hw_apb_timer_init()`；或
- generic timer：`rt_hw_gtimer_init()`。

**计划**：

- 确认 [drivers/tick.c](../drivers/tick.c) 是否被任何路径引用；
- 若无引用，加入构建排除或删除计划；
- 若保留，仅作为备用/历史参考，不参与默认固件。

### P5. pmon_gic 监控输出保持轻量

**背景**：

当前 pmon 线程用于验证 tick 和调度是否持续工作。

**计划**：

默认仅保留轻量输出：

```text
[GIC] #N Tick=N
```

避免默认打印大量 GIC 寄存器、CNTPCT、dCNT 信息，防止串口输出影响调度和中断观察。

### P6. 文档同步

**计划**：

将以下文档后续同步到 APB timer tick 方案：

- [HANDOFF_SLIM.md](../HANDOFF_SLIM.md) — 已更新 SMP 双核与 IRAM0 基线（2026-07-06）
- [TEST_METHODOLOGY.md](../TEST_METHODOLOGY.md)
- [timer_blocking_diagnosis.md](timer_blocking_diagnosis.md)
- [interrupt_group_config.md](interrupt_group_config.md)

重点更新：

- 当前 tick 源为 APB timer，而非 ARM generic timer；
- ARM generic timer 频率 mismatch 是历史排查结论，不是当前 tick 路径；
- 测试成功标准中 pmon 输出间隔应约为 2.5 秒。
