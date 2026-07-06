# HP232X SMP 单核/双核与 IRQ 调试指南

本文合并 HP232X **SMP 单核/双核 bring-up** 与 **AArch64 IRQ/异常故障定位** 的排查方法、已验证根因与 fix。

适用场景：

- 从 UP 切到 SMP 单核（`RT_USING_SMP` + `RT_CPUS_NR=1`），再扩多核；
- SMP 能 boot 到 `msh >` 但串口输入无响应或 Data abort；
- IRQ 不进入、ISR 卡死、IRQ 返回跳飞、BSS 未清零导致 hook 异常；
- 与 he200 SMP 配置对齐时的差异判断。

其它相关文档：

- 调度器历史问题：[scheduler_debug_summary.md](scheduler_debug_summary.md)
- MMU / BSS：[mmu_design.md](mmu_design.md)

---

# 第一部分：SMP 单核 bring-up

## 1. 推荐策略：先 UP，再 SMP 单核

| 阶段 | rtconfig 要点 | 验证目标 |
|------|---------------|----------|
| UP | 注释 `RT_USING_SMP`，保留 `RT_CPUS_NR 1` | tick、UART、BSS、MMU 正常 |
| SMP 单核 | `#define RT_USING_SMP`，`RT_CPUS_NR 1` | MP 调度、IRQ 延迟切换、finsh |
| SMP 多核 | `RT_CPUS_NR 2+` | secondary CPU、IPI、spin table |

**原则**：UP 已证明硬件（BSS、APB timer tick、GIC、MMU）无问题时，SMP 故障应优先查 **内核 MP 路径** 和 **rtconfig 宏**，而不是重复改 BSP boot。

### 1.1 UP 下 tick 验证（pmon）

关闭 shell，用 pmon 线程观察 tick 与 ISR 计数是否 1:1：

```c
/* rtconfig.h */
/* #define RT_USING_SMP */
#define RT_CPUS_NR 1
/* #define RT_USING_MSH */
/* #define RT_USING_FINSH */
#define RT_BSP_PMON_TEST
```

`pmon_gic.c` 每 500ms 打印 `tick` / `isr` 增量；100Hz 下预期各 **+50**。

APB timer ISR 计数见 `hp232x_apb_timer_isr_count`（`drv_apb_timer.c`）。

---

## 2. SMP 单核最小 rtconfig（对齐 he200 要点）

与 [he200/rtconfig.h](../../he200/rtconfig.h) 对比，SMP **必需** 项：

```c
#define RT_USING_SMP
#define RT_CPUS_NR 1

#define RT_USING_STDC_ATOMIC   /* 避免 timer ISR 里 rt_cpus_lock 死锁 */
#define RT_USING_HEAP_ISR      /* SMP 堆用 spinlock；finsh rt_calloc 不会在 mutex 上挂死 */

#define RT_USING_MSH
#define RT_USING_FINSH
/* ... 其余 FINSH_* 同 he200，栈按 IRAM 缩为 4096 ... */
```

**有意与 he200 不同**（非 SMP 缺项）：

| 项 | he200 | hp232x |
|----|-------|--------|
| Tick | `BSP_USING_CORETIMER` | `BSP_USING_APB_TIMER_AS_TICK` |
| 堆 | SLAB 大内存 | `RT_USING_SMALL_MEM` 48KB IRAM |
| `RT_USING_TIMER_SOFT` | 开 | 关（省 IRAM） |
| `RT_CPUS_NR` | 8 | 先 1 |

**不要** 为 SMP 再引入 hp232x 专用 kernel hook（`components.c` / `stack.c` / `shell.c` 里的 `BSP_USING_HP232X` 分支已移除或不应恢复）。

---

## 3. 排查思路（分层）

```
Layer 0: 硬件 / 早期 boot
  entry_point.S BSS、hp232x_mmu.c、board.c MMU/GIC/IPI

Layer 1: UP 内核路径
  无 RT_USING_SMP → tick / msh / 串口输入

Layer 2: SMP 配置宏
  STDC_ATOMIC + HEAP_ISR → 能否 boot、finsh 是否卡死

Layer 3: SMP 调度 / 上下文切换
  rt_system_scheduler_start、rt_hw_context_switch_to

Layer 4: IRQ 延迟切换
  tick / UART RX → rt_hw_context_switch_interrupt

Layer 5: 应用 / shell
  finsh 是否 startup、串口 help 是否响应
```

**现象 → 层映射**：

| 现象 | 优先查 |
|------|--------|
| 卡在 scheduler_start / 无 `msh` | Layer 0–3；第二部分 §12–§14 |
| 有 `msh >`，输入无反应 | Layer 4–5；§5.4、§5.6 |
| 一按键 Data abort @ `0x5` | Layer 4；§5.6 |
| `rt_calloc` / finsh 卡死 | `RT_USING_HEAP_ISR` |
| timer 不进 ISR / tick 不动 | BSS、APB timer、GIC；§15、§16 |
| 第一次 IRQ 后 PC 跳 BSS | §15（BSS 未清零） |

---

## 4. 调试手段（SMP）

### 4.1 编译与上板

```bash
export RTT_CC_PREFIX=.../aarch64-none-elf-
cd bsp/lynxi/hp232x && scons -j8
python3 remote_test.py          # 自动复位 + 抓串口
python3 test_msh_shell.py help  # 发 help，测串口输入
python3 test_msh_shell.py all   # help + list_isr（各复位一次）
```

测试服务器：`192.168.49.81`，串口 `/dev/ttyUSB1`（见 [remote_test.py](../remote_test.py)）。

### 4.2 配置切换

[test_multi.py](../test_multi.py) 可在 `up_msh` / `smp_msh` / `smp_pmon` 等预设间改 `rtconfig.h`。

### 4.3 崩溃定位

```bash
addr2line -e rtthread.elf -a -f <EPC>
```

常见 PC：

| PC 区域 | 含义 |
|---------|------|
| `mp/context_gcc.S` `rt_hw_context_switch_to` | 首次切到首个线程 |
| `mp/context_gcc.S` `rt_hw_context_switch_interrupt` | IRQ 退出时延迟切换（SMP） |
| `scheduler_mp.c` | 调度器逻辑 |
| `finsh/cmd.c` | 常为二次崩溃（symtab/backtrace），查 primary fault |

IRQ/异常逐步打印见 **第二部分 §10–§14**。

---

## 5. 已验证问题与 fix（2026-07）

### 5.1 rtconfig 宏（SMP 必需）

| 问题 | 现象 | Fix |
|------|------|-----|
| 缺 `RT_USING_STDC_ATOMIC` | timer ISR 内 `rt_cpus_lock` 死锁 | `#define RT_USING_STDC_ATOMIC` |
| 缺 `RT_USING_HEAP_ISR` | finsh `rt_calloc` 在 `rt_mutex_take` 挂死 | `#define RT_USING_HEAP_ISR` |

### 5.2 `scheduler_mp.c`：过早解锁 scheduler lock

`rt_sched_post_ctx_switch()` 在 `from_thread` 存在时无条件 `SCHEDULER_CONTEXT_UNLOCK`，首次 `rt_hw_context_switch_to` 时 `from_thread==NULL` 也会误解锁 → `_mp_scheduler_lock` 损坏 → 后续 `rt_sched_lock` 自旋。

**Fix**：仅当 `from_thread && SCHEDULER_LOCK_FLAG(pcpu)` 时解锁。

```c
if (from_thread && SCHEDULER_LOCK_FLAG(pcpu))
{
    ...
    SCHEDULER_CONTEXT_UNLOCK(pcpu);
}
pcpu->current_thread = thread;
```

### 5.3 `trap.c`：SMP 链接 UP-only 符号

SMP 构建不应引用 `rt_thread_switch_interrupt_flag` 等 UP 变量。

**Fix**：`#ifndef RT_USING_SMP` 包裹 UP debug 符号与引用。

### 5.4 `shell.c`：finsh 未 startup（hp232x hack 残留）

曾用 `#if !BSP_USING_HP232X || !RT_USING_SMP` 跳过 `rt_thread_startup(tid)`，并依赖 `components.c` 延迟启动；去掉 hook 后 **tshell 创建但不运行** → 无 `msh >` 或行为异常。

**Fix**：恢复 vanilla——`finsh_system_init()` 内始终 `rt_thread_startup(tid)`。

### 5.5 应移除的 hp232x kernel hack（勿恢复）

| 文件 | 错误做法 | 原因 |
|------|----------|------|
| `components.c` | `hp232x_smp_main_irq_*`、延迟 finsh | he200 无此逻辑；vanilla 即可 boot |
| `stack.c` | 全局 SPSR I-mask | finsh 线程 IRQ 关闭 → UART RX 死 |
| `mp/context_gcc.S` | `_context_switch_exit` 里 `DAIFSet` | 非 upstream 行为 |
| `stack_gcc.S` | `_thread_start` I-cache 维护 | 非必需 workaround |

### 5.6 【根因】`rt_hw_context_switch_interrupt` 寄存器与宏冲突

**现象**：SMP 下能到 `msh >`，**一敲键盘** Data abort，`fault addr = 0x5`，`X20 = 0x5`，PC 在 `str x0, [FROM_SPP]`。

**原因**：

- `rt_hw_context_switch_interrupt` 用 **x20** 存 `FROM_SPP`（`&from_thread->sp`）；
- 随后调用 `SAVE_CONTEXT_SWITCH_FAST`，宏内 **`mov x20, #0x05`**（EL1h SPSR 模式位，非指针）；
- x20 被覆盖为 5 → 写栈指针到地址 0x5 → Alignment fault。

**Fix**（`libcpu/aarch64/common/mp/context_gcc.S`）：

```asm
#define EXP_FRAME   x19
#define FROM_SPP    x23    /* 原 x20，避开宏 */
#define TO_SPP      x24
#define TO_TCB      x25
```

`SAVE_CONTEXT_SWITCH_FAST` 仅使用 x19/x20，不碰 x23–x25。

**路径说明**：

- `rt_hw_context_switch_to` / 线程内 `rt_hw_context_switch` **不经过**该宏 → boot 与 `msh` 提示符正常；
- UART RX / tick 触发 **IRQ 退出延迟切换** 时才进入 → 输入字符必现（fix 前）。

**与 he200**：同一套 upstream MP 代码；he200 理论上存在相同 latent bug，是否暴露取决于是否走 IRQ 延迟切换及测试覆盖。本 fix 对所有 aarch64 SMP 有效，建议合入 upstream。

---

## 6. 最终验证清单

| 项 | 命令 / 现象 | 状态 |
|----|-------------|------|
| UP + pmon tick | 每 500ms +50 tick/isr | ✅ |
| SMP boot | `Hi, this is RT-Thread!` | ✅ |
| SMP msh 提示符 | `msh >` | ✅ |
| SMP 串口输入 | `test_msh_shell.py help` → `help` 列表 | ✅ |
| 无 Data abort @ 0x5 | 按键后稳定 | ✅（x23–x25 fix 后） |
| SMP 双核 `RT_CPUS_NR=2` | CPU1 release + `[SMP] CPU1 ready` + msh 输入 | ✅（§8，2026-07-06） |

---

## 7. 涉及文件索引

| 类型 | 路径 |
|------|------|
| BSP 配置 | [rtconfig.h](../rtconfig.h) |
| BSP boot/MMU/GIC/IPI | [board.c](../drivers/board.c)、[entry_point.S](../../../libcpu/aarch64/cortex-a/entry_point.S)、[hp232x_mmu.c](../drivers/hp232x_mmu.c) |
| 内核 fix | [scheduler_mp.c](../../../src/scheduler_mp.c)、[trap.c](../../../libcpu/aarch64/common/trap.c) |
| 上下文切换 fix | [mp/context_gcc.S](../../../libcpu/aarch64/common/mp/context_gcc.S)、[include/context_gcc.h](../../../libcpu/aarch64/common/include/context_gcc.h) |
| 测试 | [remote_test.py](../remote_test.py)、[test_msh_shell.py](../test_msh_shell.py)、[pmon_gic.c](../applications/pmon_gic.c) |
| 参考 BSP | [he200/rtconfig.h](../../he200/rtconfig.h) |

---

## 8. SMP 双核（RT_CPUS_NR=2）已验证要点

2026-07 实板（192.168.49.81，`test_msh_shell.py`）在 `RT_CPUS_NR=2` 下通过：

- CPU0 启动到 `msh >`，`help` 串口输入正常；
- CPU1 release：`mbox=0x401ff00 entry=0x40401c0`（`_secondary_cpu_entry`）；
- `[SMP] CPU1 ready`（`main.c` 等待 idle 线程就绪）。

### 8.1 rtconfig 补充（相对 §2 单核）

```c
#define RT_CPUS_NR 2
#define DBG_ENABLE                  /* 否则 LOG_X 宏编译为空 */
#define RT_USING_THREADSAFE_PRINTF  /* rt_kprintf SMP 互斥（见 §8.4） */
#define RT_USING_MUTEX              /* THREADSAFE_PRINTF / heap 依赖 */
```

勿启用 `BSP_USING_HP232X_SPIN_TABLE`（会削弱 IRQ 路径）。

### 8.2 spin-table 与多核缓存一致性

BL22 从核在 bootwrapper `spin.S` 里 WFE 轮询 **`0x401ff00`**（与 [board.h](../drivers/board.h) `MBOX_ADDRESS` 一致）。

| 项 | 说明 |
|----|------|
| CPU1 mbox 偏移 | **+0**（线性 ID 1 → `(1-1)*8=0`）；勿写成 `MBOX+0x8` |
| 写入口 | `*(volatile uint64_t *)mbox = entry` |
| **必须 dcache flush** | mbox 在 IRAM0，页表为 Normal WB；CPU0 写可能只在 cache，CPU1 读不到 |
| 屏障 | `rt_hw_cpu_dcache_ops(FLUSH)` + `dsb sy`，再 `rt_hw_sev()` |
| 重复写/SEV | 一次写 + flush + SEV 即可（不必 wrapper 函数） |

实现见 [board.c](../drivers/board.c) `rt_hw_secondary_cpu_up()`。

### 8.3 从核启动路径（与 MMU）

```
bootwrapper WFE @ 0x401ff00
  → _secondary_cpu_entry (entry_point.S)
  → init_cpu_el (EL2→EL1)
  → init_cpu_stack_early（IRAM1 栈，MMU 仍关，物理直达）
  → rt_hw_secondary_cpu_bsp_start (board.c)
       → hp232x_mmu_secondary_init（MAIR/TCR/TTBR + 页表 flush）
       → arm_gic_redist_init + arm_gic_cpu_init
       → rt_system_scheduler_start()
```

要点：

- **MMU 关时** 各核可用物理地址访问 IRAM1；`entry_point` 对 IRAM1 栈用 `ldr =.secondary_cpu_stack_top`（避免 adrp 33-bit 限制）。
- **从核 MMU** 必须配齐 MAIR/TCR/TTBR，不能只做 `TTBR0`；页表由 CPU0 建好，从核 `dcache flush` 后再启用。

### 8.4 串口互斥与 LOG_X

**LOG 不打印**：`rtdbg.h` 中 `LOG_I/D/E` 依赖 `DBG_ENABLE`（或 `RT_USING_DEBUG`），与 `DBG_LVL` 无关。

**SMP 串口乱序**：多核同时 `rt_kprintf` 会交错；内核 **`RT_USING_THREADSAFE_PRINTF`**（[kservice.c](../../../src/kservice.c) `_syscon_lock`）保证**单次** `rt_kprintf` 原子。

限制：

| 路径 | 是否互斥 |
|------|----------|
| `rt_kprintf` / `rt_kputs` | ✅（开 THREADSAFE_PRINTF 后） |
| `LOG_I` 一条日志 | ⚠️ 内部 3 次 `rt_kprintf`，行内仍可能被插入 |
| `early_putc_direct` | ❌ 直写 MMIO，boot 专用 |

he200 可选 [rt_kprintf_threadsafe](../../../he200/packages/rt_kprintf_threadsafe-latest/rt_kprintf.c)（`rt_mutex` 包一层 `rt_kprintf`），与 `RT_USING_THREADSAFE_PRINTF` **二选一**；粒度同为「一次 rt_kprintf」，不能解决 LOG 多段问题。

从核 boot 阶段避免 `rt_kprintf`/`LOG_*`，待 console 稳定后再打。

### 8.5 release 前 delay 1.5s

`components.c` 顺序：`rt_components_init()`（创建 finsh）→ **`rt_hw_secondary_cpu_up()`** → `main()`。

若立刻 release CPU1，finsh 可能尚未打出 `msh >`，且 CPU1 的 LOG 与 finsh 输出字节级交错（即使有 console 锁）。

**当前权宜**：`rt_hw_secondary_cpu_up()` 开头 `rt_thread_mdelay(1500)`，让 CPU0 先完成 finsh prompt，再 SEV 唤醒 CPU1。

**后续可改**：等 `tshell` 就绪或 finsh 回调，替代固定 sleep。

### 8.6 测试

```bash
cd bsp/lynxi/hp232x && scons -j8
python3 test_msh_shell.py help     # msh + help
python3 test_msh_shell.py list_isr # IRQ 表
python3 test_msh_shell.py all      # 默认套件
python3 remote_test.py             # 启动链
```

---

## 9. （原 §8 后续项）

1. 扩更多核时递增 `cpu_release_paddr[]` 与 bootwrapper mbox 步长（每核 +8）；
2. 多核下重复 `test_msh_shell.py all` 与 tick/pmon 观测。

---

# 第二部分：IRQ 与异常故障定位

本部分记录 HP232X / AArch64 上 IRQ 与同步异常的逐步定位方法，与 SMP bring-up 共用同一套 early UART 调试基础设施。

典型案例：第一次 IRQ 后 PC 跳到 BSS `0x10005c07c` → IRAM1 `.bss` 未清零 → `rt_interrupt_enter_hook` 为垃圾值 → `blr` 跳入 BSS。修复见 §15。

---

## 10. 调试开关 `RT_KERNEL_IRQ_DBG`

### 10.1 Kconfig

AArch64 选项 `RT_KERNEL_IRQ_DBG`，定义于 [libcpu/aarch64/Kconfig](../../../libcpu/aarch64/Kconfig)：

- 打开后启用 IRQ / exception / IRQ context switch 路径上的 early UART 详细打印；
- **仅用于 bring-up**；正常版本必须关闭（打印量大，改变 IRQ 时序）。

### 10.2 rtconfig.h

```c
/* RT_KERNEL_IRQ_DBG — enable verbose early UART tracing for IRQ/exception debug */
/* #define RT_KERNEL_IRQ_DBG */
```

打开：`#define RT_KERNEL_IRQ_DBG`  
关闭：注释掉即可。

---

## 11. 打印原则

IRQ/异常路径早期**不建议**使用 `rt_kprintf()`：

1. console/heap/scheduler 可能尚未稳定；
2. `rt_kprintf()` 可能再次触发锁、调度或串口路径；
3. 栈或 BSS 损坏时 `rt_kprintf()` 可能无法输出真实现场。

使用 HP232X early UART：

- [board.c](../drivers/board.c) 的 `early_putc_direct()`；
- [trap.c](../../../libcpu/aarch64/common/trap.c) 的 `early_puts_direct()` / `early_puthex64()`。

汇编里 `bl` 调用 C 调试函数时须保存 **x0–x18、x30**（caller-saved），示例：

```asm
#ifdef RT_KERNEL_IRQ_DBG
    stp     x0, x1, [sp, #-0x10]!
    stp     x2, x30, [sp, #-0x10]!
    bl      some_debug_function
    ldp     x2, x30, [sp], #0x10
    ldp     x0, x1, [sp], #0x10
#endif
```

---

## 12. IRQ 主路径打印点

入口：[vector_gcc.S](../../../libcpu/aarch64/common/vector_gcc.S)

打开 `RT_KERNEL_IRQ_DBG` 后典型输出：

```text
IRQ entry
IRQ step=0x0000000000000001
...
IRQ get_irq ret=0x000000000000001e
IRQ handler=0x... irq=0x000000000000001e
IRQ handler done
IRQ ack done
IRQ trap exit
IRQ step=0x0000000000000006
IRQ step=0x0000000000000007
IRQ exit: ELR=0x... SPSR=0x... SP=0x...
```

### 12.1 step 含义

| 输出 | 位置 | 含义 |
|------|------|------|
| `IRQ entry` | 进入 `vector_irq` | CPU 已跳入 IRQ vector |
| `IRQ step=1` | `rt_interrupt_enter()` 前 | IRQ 栈帧已保存 |
| `IRQ step=2` | `rt_interrupt_enter()` 后 | 嵌套计数路径正常 |
| `IRQ step=3` | `SAVE_USER_CTX` 后 | 准备 C 侧 IRQ 分发 |
| `IRQ trap enter` | `rt_hw_trap_irq()` | 进入 C trap |
| `IRQ get_irq ret=...` | GIC 读 IRQ ID | 已读到 active IRQ |
| `IRQ handler=...` | 调 ISR 前 | handler 地址与 IRQ 号 |
| `IRQ handler done` | ISR 返回 | ISR 未卡死 |
| `IRQ ack done` | EOI 后 | ACK 完成 |
| `IRQ step=6` | `rt_interrupt_leave()` 后 | 嵌套计数恢复 |
| `IRQ step=7` | `rt_hw_vector_irq_sched()` 后 | SMP IRQ 尾部调度返回 |
| `IRQ exit: ELR=...` | `RESTORE_IRQ_CONTEXT` 前 | 即将 `eret` |

### 12.2 按 step 判断故障位置

| 停在哪 | 排查方向 |
|--------|----------|
| 仅 `IRQ entry` | `SAVE_IRQ_CONTEXT`、FPU 保存 |
| `step=1` 无 `step=2` | `rt_interrupt_enter()`、hook、**BSS 未清零**（§15） |
| `step=3` 无 `get_irq ret` | GIC SRE、ICC 访问权限 |
| 有 `handler=` 无 `handler done` | ISR 内死循环/异常、timer 重装 |
| 有 `handler done` 无 `ack done` | EOI、`ICC_EOIR1_EL1` |
| `IRQ exit` 后异常 | ELR/SPSR 非法、栈帧不对称（§17.5） |

---

## 13. 异常路径打印点

入口：[vector_gcc.S](../../../libcpu/aarch64/common/vector_gcc.S)、[trap.c](../../../libcpu/aarch64/common/trap.c)

```text
EXC trap entry
EXC detail: ESR=0x... EC=0x... FAR=0x... ELR=0x... SPSR=0x...
EXC regs: PC=0x... CPSR=0x... X0=0x... X30=0x...
```

| 字段 | 含义 |
|------|------|
| `ESR` / `EC` | 异常类型；`0x24/0x25` data abort |
| `FAR` | fault 地址 |
| `ELR` | 异常返回 PC；若落在 BSS/heap 则异常 |
| `SPSR` | 异常前 PSTATE（EL1h、DAIF） |
| `X30` | LR，定位调用点 |

用 `addr2line` / `nm` / `rtthread.map` 查 ELR 所属段。BSS 地址示例：

```bash
cd bsp/lynxi/hp232x
aarch64-none-elf-nm -n rtthread.elf | grep -E '10005c|rt_interrupt'
```

---

## 14. IRQ 尾部调度与上下文切换

### 14.1 SMP 路径

SMP 在 [vector_gcc.S](../../../libcpu/aarch64/common/vector_gcc.S) 的 `vector_irq` 末尾调用 `rt_hw_vector_irq_sched` → [mp/vector_gcc.S](../../../libcpu/aarch64/common/mp/vector_gcc.S) → `rt_scheduler_do_irq_switch()`。

当 tick/UART 等在 ISR 内触发 `rt_schedule()` 且 `irq_nest>0` 时，设 `irq_switch_flag=1`；IRQ 退出时在 **`rt_hw_context_switch_interrupt`**（[mp/context_gcc.S](../../../libcpu/aarch64/common/mp/context_gcc.S)）完成切换。

UP 路径使用 `rt_thread_switch_interrupt_flag` + `rt_hw_context_switch_interrupt_do`（[up/context_gcc.S](../../../libcpu/aarch64/common/up/context_gcc.S)），SMP 不经过该路径。

### 14.2 观察重点

1. `from` / `to` 是否为 TCB 中 `sp` 字段的**地址**（非 sp 值本身）；
2. 切换后 `new_sp` 是否在线程栈范围；
3. 栈帧 `ELR/SPSR` 是否合法；
4. **SMP**：`FROM_SPP` 不可与 `SAVE_CONTEXT_SWITCH_FAST` 共用 x20（见第一部分 §5.6）。

---

## 15. 案例：IRQ 后 PC 跳 BSS `0x10005c07c`

### 15.1 现象

第一次 timer IRQ 后 PC 固定为 `0x10005c07c`（BSS 内 `uart0_addr+4`）。

### 15.2 日志特征

```text
IRQ entry
IRQ step=0x0000000000000001
EXC detail: ... ELR=0x000000010005c07c ... X30=0x000000000405a818
```

- 停在 `step=1` 与 `step=2` 之间 → 故障在 `rt_interrupt_enter()`；
- `X30` 指向 hook 调用返回点 → `rt_interrupt_enter_hook` 非零垃圾 → `blr` 跳 BSS。

### 15.3 根因

[entry_point.S](../../../libcpu/aarch64/cortex-a/entry_point.S) 对高地址 BSS 曾跳过 early 清零，C 阶段使用前未补清。

### 15.4 修复

在 [entry_point.S](../../../libcpu/aarch64/cortex-a/entry_point.S) 的 `init_kernel_bss` 清零 BSS（当前 hp232x 已采用）；或在 `rt_hw_board_init()` 最早处：

```c
rt_memset((void *)bss_start, 0, bss_end - bss_start);
```

须在使用 `rt_interrupt_*_hook`、`earlycon_base`、调度器全局状态**之前**执行。

### 15.5 修复后

IRQ 能连续走完 step 1→7，ELR 回到 `.text` 合法地址。

---

## 16. 常见故障模式速查

| 现象 | 排查 |
|------|------|
| 无 `IRQ entry` | DAIF.I、VBAR、GIC enable、Group1NS、HCR_EL2.IMO |
| 仅到 `step=1` | `rt_interrupt_enter`、hook、**BSS** |
| ISR 不返回 | ISR 内 `rt_kprintf`、heap、timer 重装 |
| 第一次 IRQ 后不再触发 | timer reload、EOI、DAIF 被线程 SPSR 屏蔽 |
| IRQ 返回跳飞 | SAVE/RESTORE 对称、FPU、`x30` 调试保存、切换栈帧 |
| SMP 按键 @ `0x5` | §5.6，`FROM_SPP` 与宏 x20 冲突 |

---

## 17. 调试结束后的清理

```c
/* #define RT_KERNEL_IRQ_DBG */
```

```bash
cd bsp/lynxi/hp232x && scons -j8 && python3 remote_test.py
```

长期开启会掩盖真实竞态；SMP 串口输入测试请用 `test_msh_shell.py` 在**关闭** IRQ 调试的前提下验证。

---

## 18. 变更记录

| 日期 | 内容 |
|------|------|
| 2026-07-06 | 初版 SMP 单核 bring-up |
| 2026-07-06 | 合并 IRQ 异常定位章节；文件名为 `smp_irq_debug.md` |
| 2026-07-06 | §8 SMP 双核：spin-table 缓存一致性、dcache flush、串口 THREADSAFE_PRINTF、release 前 1.5s delay；实板 `test_msh_shell.py` PASS |
