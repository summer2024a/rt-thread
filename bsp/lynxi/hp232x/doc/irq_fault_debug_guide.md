# HP232X IRQ Fault Debug Guide

本文记录 HP232X / AArch64 IRQ 故障定位方法，重点覆盖：

- 中断入口是否进入；
- 卡在 IRQ 流程的哪个阶段；
- handler 是否执行；
- EOI/ACK 是否完成；
- IRQ 返回时 ELR/SPSR/SP 是否正确；
- 中断触发后同步异常如何读取 ESR/FAR/ELR/SPSR；
- 如何通过 `RT_KERNEL_IRQ_DBG` 打开/关闭这些定位打印。

该方法来自一次实际问题定位：第一次 IRQ 后 PC 固定跳到 BSS 地址 `0x10005c07c`。最终定位为 IRAM1 高地址 `.bss` 未在 C 阶段清零，导致 `rt_interrupt_enter_hook` 是垃圾值，`rt_interrupt_enter()` 执行 hook 时 `blr` 跳入 BSS。

---

## 1. 调试开关

### 1.1 Kconfig

AArch64 下新增调试开关：

```text
RT_KERNEL_IRQ_DBG
```

位置：

- [libcpu/aarch64/Kconfig](../../../../libcpu/aarch64/Kconfig)

含义：

- 打开后启用 IRQ / exception / IRQ context switch 路径上的 early UART 详细打印；
- 默认关闭；
- 只用于 bring-up 和故障定位；
- 正常版本必须关闭，因为打印非常多，会改变 IRQ 时序。

### 1.2 rtconfig.h 手动开关

HP232X 当前也在 [rtconfig.h](../rtconfig.h) 中保留了手动开关：

```c
/* RT_KERNEL_IRQ_DBG — enable verbose early UART tracing for IRQ/exception debug */
/* #define RT_KERNEL_IRQ_DBG */
```

打开方法：

```c
#define RT_KERNEL_IRQ_DBG
```

关闭方法：

```c
/* #define RT_KERNEL_IRQ_DBG */
```

---

## 2. 打印原则

IRQ/异常路径早期不建议使用 `rt_kprintf()`，原因：

1. console/heap/scheduler 可能尚未完全稳定；
2. `rt_kprintf()` 本身可能再次触发锁、调度或串口路径；
3. 异常路径中如果栈或 BSS 已损坏，`rt_kprintf()` 可能无法输出真实现场。

本调试方案使用 HP232X 的 early UART：

- [board.c](../drivers/board.c) 中的 `early_putc_direct()`；
- [trap.c](../../../../libcpu/aarch64/common/trap.c) 中的 `early_puts_direct()` / `early_puthex64()`。

汇编里调用 C 调试函数时必须注意：

- AArch64 ABI 中 `x0-x18` 是 caller-saved；
- `bl` 会覆盖 `x30`；
- 如果汇编函数后续还要 `ret`，或者还要使用 `x0/x1/x2` 等入参，必须先保存再恢复。

典型写法：

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

## 3. IRQ 主路径打印点

IRQ 主入口在：

- [vector_gcc.S](../../../../libcpu/aarch64/common/vector_gcc.S)

打开 `RT_KERNEL_IRQ_DBG` 后，IRQ 路径会输出：

```text
IRQ entry
IRQ step=0x0000000000000001
IRQ step=0x0000000000000002
IRQ step=0x0000000000000003
IRQ trap enter
IRQ get_irq call
IRQ get_irq ret=0x000000000000001e
IRQ handler=0x... irq=0x000000000000001e
IRQ handler done
IRQ ack done
IRQ trap exit
IRQ step=0x0000000000000004
IRQ step=0x0000000000000005
IRQ step=0x0000000000000006
irq_sched flag=0x...
IRQ step=0x0000000000000007
IRQ exit: ELR=0x... SPSR=0x... SP=0x...
stack[0..1]=0x... 0x...
```

### 3.1 step 含义

| 输出 | 位置 | 含义 |
|---|---|---|
| `IRQ entry` | IRQ 刚进入 `vector_irq` | CPU 已跳入 IRQ vector |
| `IRQ step=1` | `rt_interrupt_enter()` 前 | IRQ 栈帧已保存，准备增加中断嵌套计数 |
| `IRQ step=2` | `rt_interrupt_enter()` 后 | 中断嵌套计数路径正常返回 |
| `IRQ step=3` | `SAVE_USER_CTX` 后 | 准备进入通用 IRQ 分发 |
| `IRQ trap enter` | `rt_hw_trap_irq()` 开始 | 进入 C 侧 IRQ trap |
| `IRQ get_irq call` | `rt_hw_interrupt_get_irq()` 前 | 准备读取 GIC active IRQ |
| `IRQ get_irq ret=...` | `rt_hw_interrupt_get_irq()` 后 | 已读到 GIC IRQ ID |
| `IRQ handler=... irq=...` | 调 handler 前 | handler 地址和 IRQ 号正确性检查 |
| `IRQ handler done` | handler 返回后 | ISR 本体没有卡死 |
| `IRQ ack done` | `rt_hw_interrupt_ack()` 后 | EOI/ACK 已完成 |
| `IRQ trap exit` | `rt_hw_trap_irq()` 结束 | C 侧 IRQ 分发完成 |
| `IRQ step=4` | `rt_hw_trap_irq()` 后 | IRQ handler 完整返回 |
| `IRQ step=5` | `RESTORE_USER_CTX` 后 | 用户上下文恢复阶段完成 |
| `IRQ step=6` | `rt_interrupt_leave()` 后 | 中断嵌套计数恢复完成 |
| `irq_sched flag=...` | `rt_hw_vector_irq_sched()` | 是否需要 IRQ 尾部调度切换 |
| `IRQ step=7` | `rt_hw_vector_irq_sched()` 后 | IRQ 尾部调度路径返回 |
| `IRQ exit: ELR=...` | `RESTORE_IRQ_CONTEXT` 前 | 即将 `eret` 的返回现场 |

### 3.2 如何根据 step 判断故障位置

- 只看到 `IRQ entry`，没有 `IRQ step=1`：
  - `SAVE_IRQ_CONTEXT` 或 early debug 本身异常；
  - 检查 IRQ 栈、`SAVE_IRQ_CONTEXT` 宏、FPU 保存权限。

- 看到 `IRQ step=1`，没有 `IRQ step=2`：
  - 故障在 `rt_interrupt_enter()` 内；
  - 重点检查 `rt_interrupt_nest`、`rt_interrupt_enter_hook`、BSS 是否清零。

- 看到 `IRQ step=3`，没有 `IRQ get_irq ret`：
  - 故障在 GIC active IRQ 读取；
  - 检查 ICC 系统寄存器访问权限、GICv3 SRE、EL 路由。

- 看到 `IRQ handler=...`，没有 `IRQ handler done`：
  - ISR 本身卡死或异常；
  - 对 timer ISR，检查是否在 ISR 中使用 `rt_kprintf()`、是否重装 timer、是否访问非法内存。

- 看到 `IRQ handler done`，没有 `IRQ ack done`：
  - 故障在 EOI/ACK；
  - 检查 `ICC_EOIR1_EL1` 写入、IRQ ID、Group 配置。

- 看到 `IRQ exit: ELR=...` 后异常：
  - 检查 `ELR` 是否在 `.text` 范围；
  - 检查 `SPSR` 是否是 EL1h 且 DAIF 符合预期；
  - 检查 IRQ 栈帧保存/恢复是否对称。

---

## 4. 异常路径打印点

同步异常入口在：

- [vector_gcc.S](../../../../libcpu/aarch64/common/vector_gcc.S)
- [trap.c](../../../../libcpu/aarch64/common/trap.c)

打开 `RT_KERNEL_IRQ_DBG` 后，异常路径会输出：

```text
ENTRY step=0x0000000000000010
ENTRY step=0x0000000000000011
EXC trap entry
EXC detail: ESR=0x... EC=0x... FAR=0x... ELR=0x... SPSR=0x...
EXC regs: PC=0x... CPSR=0x... X0=0x... X30=0x...
ENTRY step=0x0000000000000012
ENTRY step=0x0000000000000013
ENTRY step=0x0000000000000014
```

### 4.1 异常字段含义

| 字段 | 含义 | 观察方法 |
|---|---|---|
| `ESR` | 异常综合状态 | 用 EC 判断异常类型 |
| `EC` | Exception Class | `0x20/0x21` instruction abort，`0x24/0x25` data abort，`0x22` PC alignment fault |
| `FAR` | Fault Address | data/instruction abort 时重点看 |
| `ELR` | 异常返回地址 | 是否落在 `.text`，是否跳到 BSS/heap/stack |
| `SPSR` | 异常前 PSTATE | 判断 EL1h/EL0t、DAIF 屏蔽位 |
| `X30` | LR | 判断异常发生在哪个调用点之后 |

### 4.2 用 `ELR` 定位非法 PC

如果看到：

```text
ELR=0x000000010005c07c
```

先查该地址属于哪个段/符号：

```bash
cd /work/rt-thread/bsp/lynxi/hp232x
/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-nm -n rtthread.elf | grep -E '10005c|uart0_addr|earlycon_base|rt_interrupt'
rg -n '10005c07c|uart0_addr|earlycon_base|rt_interrupt' rtthread.map
```

本次案例中：

```text
0x10005c078 B uart0_addr
0x10005c080 B earlycon_base
0x10005caa8 B rt_interrupt_nest
0x10005cab0 b rt_interrupt_enter_hook
```

`0x10005c07c = uart0_addr + 4`，明显是 BSS，不是代码。

再结合：

```text
X30=0x000000000405a818
```

反汇编确认 `0x405a818` 是 `rt_interrupt_enter()` 中 hook 调用后的返回点，说明 CPU 是从 `rt_interrupt_enter_hook` 跳入了垃圾地址。

---

## 5. IRQ 尾部调度打印点

IRQ 尾部调度在：

- [up/vector_gcc.S](../../../../libcpu/aarch64/common/up/vector_gcc.S)

打开 `RT_KERNEL_IRQ_DBG` 后输出：

```text
irq_sched flag=0x0000000000000000
```

含义：

- `0`：本次 IRQ 不触发线程切换；
- `1`：本次 IRQ 结束时会进入 `rt_hw_context_switch_interrupt_do()`。

如果 `irq_sched flag=1` 后异常或卡死，继续看 IRQ context switch 打印。

---

## 6. IRQ 中线程切换打印点

IRQ 中调度切换在：

- [up/context_gcc.S](../../../../libcpu/aarch64/common/up/context_gcc.S)
- [context_gcc.h](../../../../libcpu/aarch64/common/include/context_gcc.h)

打开 `RT_KERNEL_IRQ_DBG` 后输出：

```text
ctx_switch_irq before: flag=0x... from=0x... to=0x...
ctx_switch_irq after:  flag=0x... from=0x... to=0x...
ctx_switch_irq_do before
ctx_switch_irq_do after new_sp=0x...
```

观察重点：

1. `from` / `to` 是否是线程 TCB 中的 `sp` 字段地址；
2. `new_sp` 是否落在线程栈范围；
3. `new_sp` 指向的栈帧前两项是否是合法 `ELR/SPSR`；
4. `SPSR` 是否保存真实 DAIF，而不是固定屏蔽 IRQ。

注意：汇编调试调用必须保存 `x30`，否则 `ret` 会返回到调试调用之后，而不是原调用者。

---

## 7. 本次 `0x10005c07c` 问题定位流程

### 7.1 现象

第一次 timer IRQ 后，PC 固定跳到：

```text
0x10005c07c
```

### 7.2 打开调试

在 [rtconfig.h](../rtconfig.h) 中打开：

```c
#define RT_KERNEL_IRQ_DBG
```

重新编译并运行：

```bash
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build && scons -j$(nproc)
python3 remote_test.py
```

### 7.3 观察日志

关键日志：

```text
IRQ entry
IRQ step=0x0000000000000001
EXC detail: ESR=0x0000000002000000 EC=0x0000000000000000 FAR=0xd51e4100d51c4100 ELR=0x000000010005c07c SPSR=0x00000000200003c5
EXC regs: PC=0x000000010005c07c CPSR=0x00000000200003c5 X0=0x000000010005cc78 X30=0x000000000405a818
```

判断：

1. `IRQ step=1` 后未到 `IRQ step=2`，故障在 `rt_interrupt_enter()`；
2. `ELR=0x10005c07c` 落在 BSS；
3. `X30=0x405a818` 是 `rt_interrupt_enter()` 中 hook 调用之后；
4. 说明 `rt_interrupt_enter_hook` 本应为 0，但实际是垃圾值，执行 `blr hook` 跳到 BSS。

### 7.4 根因

[entry_point.S](../../../../libcpu/aarch64/cortex-a/entry_point.S) 中因 `__bss_start` 位于 4GB 以上，跳过早期 BSS 清零：

```asm
.bss_skip_clean:
    /* BSS in high memory (>33-bit), skip early zero. Will be zeroed by C code. */
    ret
```

但 C 阶段之前没有补清 `.bss`，导致 BSS 全局变量保留旧值/垃圾值。

### 7.5 修复

在 [board.c](../drivers/board.c) 的 `rt_hw_board_init()` 早期补清：

```c
rt_memset((void *)bss_start, 0, bss_end - bss_start);
rt_hw_earlycon_ioremap_early();
```

注意必须在使用这些 BSS 全局变量之前执行：

- `rt_interrupt_enter_hook`；
- `rt_interrupt_leave_hook`；
- `earlycon_base`；
- scheduler / interrupt 全局状态。

### 7.6 修复后验证

修复后日志：

```text
IRQ entry
IRQ step=0x0000000000000001
IRQ step=0x0000000000000002
IRQ step=0x0000000000000003
IRQ trap enter
IRQ get_irq ret=0x000000000000001e
IRQ handler=0x... irq=0x000000000000001e
IRQ handler done
IRQ ack done
IRQ trap exit
IRQ step=0x0000000000000004
IRQ step=0x0000000000000005
IRQ step=0x0000000000000006
irq_sched flag=0x0000000000000000
IRQ exit: ELR=0x0000000004048f1c SPSR=0x0000000080000005 SP=0x000000010005c690
```

结论：

- `0x10005c07c` 不再出现；
- IRQ 30 能连续进入；
- handler 能执行并返回；
- ACK/EOI 完成；
- IRQ 返回 ELR 是 `.text` 中的合法地址。

---

## 8. 常见故障模式速查

### 8.1 IRQ 完全不进入

现象：没有 `IRQ entry`。

排查：

1. DAIF.I 是否为 0；
2. `VBAR_EL1` 是否指向 `system_vectors`；
3. GICD/GICR/ICC_IGRPEN1_EL1 是否启用；
4. PPI/SPI group 是否为 Group1NS；
5. HCR_EL2.IMO 是否为 0，IRQ 是否路由到 EL1。

### 8.2 IRQ 只到 step=1

现象：有 `IRQ step=1`，没有 `IRQ step=2`。

排查：

1. `rt_interrupt_enter()`；
2. `rt_interrupt_nest` 地址和值；
3. `rt_interrupt_enter_hook` 是否为 0 或合法函数；
4. BSS 是否清零。

### 8.3 handler 不返回

现象：有 `IRQ handler=...`，没有 `IRQ handler done`。

排查：

1. ISR 是否调用复杂打印；
2. ISR 是否访问 heap/未初始化对象；
3. Timer 是否正确重装；
4. 是否出现嵌套异常。

### 8.4 ACK 后不再触发

现象：第一次完整处理，后续无 IRQ。

排查：

1. `CNTP_CTL_EL0` 是否重新 enable；
2. `CNTP_TVAL_EL0` / `CNTP_CVAL_EL0` 是否重装；
3. `ICC_EOIR1_EL1` 写入 IRQ ID 是否正确；
4. GIC priority mask 是否允许；
5. DAIF.I 是否在线程切换后被恢复成 1。

### 8.5 IRQ 返回后跳飞

现象：`IRQ exit: ELR=...` 之后异常，或 ELR 是 BSS/heap/stack。

排查：

1. `SAVE_IRQ_CONTEXT` / `RESTORE_IRQ_CONTEXT` 是否对称；
2. FPU Q0-Q31 是否保存/恢复一致；
3. 汇编调试 `bl` 是否保存 `x30`；
4. 线程切换栈帧的 `ELR/SPSR` 是否正确；
5. `SAVE_CONTEXT_SWITCH` 是否保存真实 DAIF。

---

## 9. 调试结束后的清理

定位完成后必须关闭：

```c
/* #define RT_KERNEL_IRQ_DBG */
```

然后重新编译验证：

```bash
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build && scons -j$(nproc)
python3 remote_test.py
```

关闭原因：

- IRQ 打印量极大；
- 串口输出会显著改变中断时序；
- early UART 轮询输出会延长 ISR 时间；
- 长期保留会掩盖真实竞态和时序问题。
