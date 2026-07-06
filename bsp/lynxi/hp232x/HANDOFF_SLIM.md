# HP232X BSP Handoff 精简版

## 当前状态（2026-07-06）

### ✅ SMP 双核（RT_CPUS_NR=2）实板验证通过

BL22 启动链 + SMP 双核 + msh shell 已在测试板（192.168.49.81）验证：

```text
msh >help          # 串口输入正常
[SMP] release CPU1 mbox=0x401ff00 entry=0x40401c0
[SMP] CPU1 ready (idle=0x...)
msh >list_isr      # apb_tick / uart0 / IPI 计数正常
```

**关键 fix（详见 [doc/smp_irq_debug.md](doc/smp_irq_debug.md) §8）**：

| 问题 | Fix |
|------|-----|
| CPU1 mbox 偏移错误 | `HP232X_CPU_RELEASE_MBOX(cpu_id)`，CPU1 → +0 |
| CPU1 读不到 release | mbox 写后 **dcache flush** + `dsb sy` + `SEV` |
| 从核 MMU 不完整 | `hp232x_mmu_secondary_init()` 配齐 MAIR/TCR/TTBR |
| 从核 GIC | `arm_gic_redist_init` + `arm_gic_cpu_init` |
| 串口乱序 | `RT_USING_THREADSAFE_PRINTF` |
| finsh 与 CPU1 竞争 | `rt_hw_secondary_cpu_up()` 前 **delay 1.5s**（权宜） |

**当前 rtconfig 要点**：

```c
#define RT_USING_SMP
#define RT_CPUS_NR 2
#define DBG_ENABLE
#define RT_USING_THREADSAFE_PRINTF
#define RT_USING_INTERRUPT_INFO   /* list_isr，+~8KB IRAM1 BSS */
```

### ⚠️ 内存压力：IRAM0 仅剩 ~20 KB

SMP 双核 + shell + IRQ 统计后，**IRAM0 是下一阶段的瓶颈**：

```text
Section                   Size KB      End addr
.head                        1.05  0x04040450
.text+.rodata+symtab       205.34  0x04073D60
.data                        3.35  0x04074C20
.mmu_table                  24.00  0x0407B000   ← 已从 12KB 增至 24KB
------------------------------------------------
IRAM0 已用                  234.07 KB / 256 KB  (91.4%)
IRAM0 剩余（连续）           20.00 KB
IRAM1 BSS                    69.61 KB / 256 KB  (27.2%，尚有余量)
```

BL22 内核链接窗口：`0x04040000` – `0x0407FFFF`（前 256KB `0x04000000–0x0403FFFF` 留给 bootwrapper）。

构建后复查：

```bash
cd bsp/lynxi/hp232x && scons -j8
python3 mkimage.py rtthread.bin rtthread-header.bin --dest-addr 0x04040000 --elf rtthread.elf
```

### 🔜 下一步：IRAM0 内存裁剪

优先方向（见 [doc/system_trim_plan.md](doc/system_trim_plan.md)）：

1. **`.text`（~205 KB）**：裁剪 finsh 命令、关闭未用组件、`-ffunction-sections` 已开则查 map 大符号
2. **`.mmu_table`（24 KB）**：评估页表粒度/映射范围能否压缩
3. **功能开关**：SMP 稳定后可评估是否关闭 `RT_USING_INTERRUPT_INFO`（省 ~8KB IRAM1，对 IRAM0 无直接帮助）
4. **勿重复**：mm 组件移除已失败（见 [doc/mm_component_removal_attempt.md](doc/mm_component_removal_attempt.md)）

目标：IRAM0 使用率降到 **≤85%**（留出 ≥38 KB 余量），以支撑后续驱动/功能扩展。

---

## 历史：单核 UP 基准（2026-07-05，仍有效）

**关键发现**：

通过添加调试代码和清零 caller-saved 寄存器，已确认：

| 步骤 | 状态 | 说明 |
|------|------|------|
| rt_cpus_lock_status_restore 调用 | ✅ 成功 | 调试输出 'B' 和 'A' 都出现 |
| caller-saved 寄存器清零 | ✅ 成功 | X0-X18 全零，不再访问垃圾地址 |
| 栈恢复 | ✅ 成功 | X19 = main_thread_entry（正确） |
| eret 执行 | ✅ 成功 | 进入 _thread_start |
| **_thread_start 执行** | ❌ SError | 外部错误，不是指令错误 |

**调试输出**：
```
BCP1S14BACSError
```
- B = Before rt_cpus_lock_status_restore ✓
- A = After rt_cpus_lock_status_restore ✓
- C = Clear caller-saved registers ✓
- 然后触发 SError

**SError 可能原因**：
1. 缓存一致性配置问题
2. TLB/MMU 配置问题
3. CPU errata 未修复（Cortex-A53/A57/A72）
4. 外部中断/总线错误

**当前结论**：
- ✅ UP 模式完全正常，可用于开发
- ❌ SMP 模式存在硬件级问题，需要 JTAG 调试或更深入的架构分析

**下一步建议**：
1. 检查缓存配置（是否需要在 entry_point.S 添加缓存初始化）
2. 添加 CPU errata 修复（参考 u-boot SPL 的 apply_core_errata）
3. 或者暂时使用 UP 模式，等待 RT-Thread 上游支持
- ✅ eret执行成功
- ❌ `_thread_start`执行时触发**SError异常**
- ❌ 可能是FPU状态恢复问题（`RESTORE_FPU sp`）
- ❌ 或者SMP模式下寄存器/FPU状态管理有根本差异

**建议**：
- 当前使用 **UP模式**（单核）进行开发
- SMP问题需要更深入的架构分析（可能需要JTAG调试FPU状态）
- 或者等待RT-Thread上游修复SMP相关问题

---

## SError 崩溃分析（2026-07-06）

### 崩溃现场

```
SError
Exception:
X19:0x00000000000405bf34  → main_thread_entry ✅
X20:0x000000000004062b24  → _thread_exit ✅
X29:0x(nil)               → 栈帧指针=0 (AAPCS64规范)
SP_EL0:0x(nil)            → 用户态栈=0 (规范)
SPSR:0x05                 → EL1h模式 (EL1, SP_EL1)
EPC:0x00000000000404e61c  → _thread_start (stack_gcc.S:25)
```

**当前线程**: main (pri=1, running, stack=0x100062090 in IRAM1)

**崩溃指令**: `_thread_start` 中的 `blr x19` — 分支到 `main_thread_entry(0x405bf34)` 时触发 SError

### 根因分析

**最可能原因：I-cache 一致性导致硬件总线错误**

在 [hp232x_mmu.c:157-174](../drivers/hp232x_mmu.c#L157-L174) 中：
```c
// 禁用 I-cache → 启用MMU → 重新启用 I-cache
sctlr &= ~0x1000UL;   // disable instruction cache
// ... enable MMU ...
sctlr |= 0x1000UL;    // enable instruction cache  ← 之前没有 ic ialluis!
```

**问题链路**：
1. 内核代码在 MMU 禁用时执行（bootwrapper 通过物理地址加载）
2. MMU 禁用 I-cache → 启用 MMU 和页表 → 重新启用 I-cache
3. **I-cache 重新启用前未执行 `ic ialluis` 清除**，导致缓存中有旧的不一致条目
4. 当 `_thread_start` 执行 `blr x19` 取指 `main_thread_entry(0x0405bf34)` 时
5. I-cache miss 触发页表翻译，但缓存不一致导致发送给总线的物理地址错误
6. 总线 interconnect 报告 **SError**（硬件系统级异步错误，非MMU翻译失败）

**为什么是 SError 而不是 Translation Fault**：
- Translation fault：MMU 找不到有效页表项 → 触发 Data Abort / Instruction Abort
- SError：总线 interconnect 层面的硬件错误 → 通常由错误的物理地址、时钟/电源门控、或缓存一致性破坏引起

### 修复方案

在 [hp232x_mmu.c:169](../drivers/hp232x_mmu.c#L169) 启用 I-cache 之前，添加 I-cache 失效：

```c
LOG_I("[mmu] Invalidate I-cache before enabling");
__asm__ volatile("ic ialluis" ::: "memory");  // Invalidate I-cache for all CPUs
__asm__ volatile("dsb sy" ::: "memory");
__asm__ volatile("isb" ::: "memory");

__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
sctlr |= 0x1000UL;    // enable instruction cache
```

### 已完成的修复（2026-07-04）

### 1. 🔥 BSS 清零时机修复（重���突破）

**问题根因**：
- HP232X BSS 位于 IRAM1 `0x10005a000` (bit 32 = 1)
- `entry_point.S` 使用 `adrp` 指令，受 33 位限制，跳过清零
- SMP spinlock 在 BSS 清零前初始化，使用垃圾值崩溃

**修复方案**：
- `entry_point.S:393` - 使用 `ldr` 加载 64 位绝对地址
- `board.c:169` - 移除重复的 BSS 清零
- 确保 BSS 在 `rtthread_startup` 前清零

**验证结果**：
- ✅ 单核模式完全启动成功
- ✅ SMP 模式 banner 正常显示（不卡在 IRQ step 1）

**相关 memory**: [hp232x-bss-zeroing-fix-2026-07-04.md](../../memory/hp232x-bss-zeroing-fix-2026-07-04.md)

### 3. SMP 调试深度分析（发现问题）

**调试历程**（2026-07-04 13:00-17:14）：

通过逐步添加调试输出，完整追踪了SMP启动流程：

1. **初始化流程完整** ✅：
   ```text
   TSAILGUDLHR  ← Timer/Scheduler/Application/Idle/Lock/Go 全部执行
   CPUID=0 MPIDR=80000000 ← CPU ID正常
   SP=0x100062f40 ← 线程栈正确
   ```

2. **eret 执行成功** ✅：
   - ELR_EL1 = `_thread_start` (正确地址)
   - SPSR_EL1 = 0x05 (EL1h模式，正确)
   - eret 成功跳转到 `_thread_start`

3. **_thread_start 执行失败** ❌：
   ```text
   SError 异常
   EPC: 0x404e0d4  ← _thread_start地址
   
   后续触发：
   Data abort
   fault addr = 0xaa1f03fe940000e5  ← 完全随机的错误地址
   ```

**问题定位**：
- 系统成功完成初始化和上下文切换准备
- eret 执行并跳转到 `_thread_start`
- 但在执行 `_thread_start` 时触发 SError 异常
- 可能是 FPU 状态恢复问题（`RESTORE_FPU sp`）
- 或者 SMP 模式下的寄存器/FPU 状态管理差异

**调试代码问题**：
- 发现调试代码UART地址计算错误（mov/movk指令）
- 修正后仍无法解决根本问题（SError异常）

**最终结论**：
- ✅ UP 模式完全稳定
- ❌ SMP 模式存在架构级问题，需要更深入分析
- 建议：暂时使用 UP 模式开发，SMP 问题待后续深入研究

### 4. SMP vs UP 理论分析（2026-07-04）

**关键发现：SMP和UP使用不同的上下文切换实现**

#### SMP调试进展（2026-07-05）

**调试输出分析**：
```
P2I0     ← PARange=2(40-bit支持), IPS=0(32-bit默认)
IBKSUE   ← init_cpu_el, init_kernel_bss, init_cpu_stack_early成功
123456789ABC ← rtthread_startup各步骤成功
S1       ← 栈在IRAM1 (bit32=1)
SError   ← 在_thread_start执行时触发
```

**确认的事实**：
1. ✅ IPS默认值=0，MMU启用后修正为2
2. ✅ 栈在IRAM1正确位置（bit32=1）
3. ✅ 所有初始化步骤成功执行
4. ❌ 崩溃时X19/X20寄存器值不正确

**崩溃时寄存器分析**：
| 寄存器 | 实际值 | 应该值 | 差异 |
|--------|--------|--------|------|
| X19 | 0x405bf34 | main_thread_entry地址 | 错误！超出components.o范围 |
| X20 | 0x4062b24 | 线程退出函数地址 | 错误！|
| X21 | 0 | 参数 | 正确 |

**栈初始化调试输出**：
```
T0405BE → tentry地址bit[31:12] = 0x0405BExxx
V0405B  → 栈上存储值bit[31:12] = 0x0405Bxxx (E→F变化)
崩溃X19 = 0x405bf34 → bit[31:12] = 0x0405BFxxx
```

**差值**：BFxxx - BExxx = 0x1000 (4KB = stack_size)

**关键发现**：
- 栈初始化写入tentry参数地址
- 栈上实际存储的值差了4KB（正好是stack_size）
- 说明栈帧偏移计算有问题

**问题定位**：
- rt_hw_stack_init返回的sp值正确
- 但恢复时使用的sp可能偏移了4KB
- 或者栈帧偏移SP+0x70计算错误

**待验证**：
1. 传入rt_hw_context_switch_to的sp参数值
2. 恢复时实际使用的sp是否等于初始化返回的sp
3. 检查栈帧偏移计算

#### 启动流程差异对比

| 阶段 | 操作 | UP模式 | SMP模式 | 潜在问题 |
|------|------|---------|---------|----------|
| 1 | rt_hw_spin_lock_init(&_cpus_lock) | 无 | 调用 | MMU启用前访问IRAM1 |
| 2 | rt_hw_board_init() → hp232x_mmu_init() | 调用 | 调用 | MMU启用 |
| 9 | rt_hw_spin_lock(&_cpus_lock) | 无 | 调用 | 锁损坏可能死循环 |
| 10 | rt_hw_context_switch_to() | UP版本 | SMP版本 | 调用链更长 |

#### rt_hw_context_switch_to 差异

**UP版本** (`up/context_gcc.S:44-48`):
```asm
rt_hw_context_switch_to:
    clrex
    ldr     x0, [x0]           // 加载栈指针
    RESTORE_CONTEXT_SWITCH x0  // 直接恢复上下文
    NEVER_RETURN
```

**SMP版本** (`mp/context_gcc.S:41-55`):
```asm
rt_hw_context_switch_to:
    ldr     x0, [x0]           // 加载栈指针
    mov     sp, x0             // 设置SP
    update_tidr x1             // 设置tpidr (线程自指针)
    mov     x19, x1            // 保存to_thread

    mov     x0, x19
    bl      rt_cpus_lock_status_restore  // 恢复锁状态
    b       _context_switch_exit         // 恢复上下文并eret
```

**SMP版本调用链**:
```
rt_hw_context_switch_to
  → rt_cpus_lock_status_restore
    → rt_sched_post_ctx_switch
      → rt_cpu_self()
        → rt_hw_cpu_id()      // 读MPIDR，纯寄存器操作
        → &_cpus[cpu_id]      // 访问IRAM1 (BSS段)
```

#### 核心变量存储位置

| 变量 | 定义位置 | 存储段 | 地址 |
|------|----------|--------|------|
| `_cpus_lock` | cpu_mp.c:25 | BSS | IRAM1 (0x10005a000+) |
| `_cpus[]` | cpu_mp.c:24 | BSS | IRAM1 |
| `_mp_scheduler_lock` | scheduler_mp.c:47 | BSS | IRAM1 |

#### 可能的问题根因

**假设1: MMU启用前访问IRAM1问题**

时序分析:
```
rtthread_startup():
  1. rt_hw_spin_lock_init(&_cpus_lock)  ← 第1步，MMU未启用
     - atomic_store_explicit(&lock->_value, 0, ...)
     - _cpus_lock在IRAM1 (高地址 0x10005a000)
     
  2. rt_hw_local_irq_disable()
  
  3. rt_hw_board_init()
     - hp232x_mmu_init() ← MMU在此启用
```

如果IPS默认值不足:
- CPU无法访问 >4GB 物理地址
- IRAM1地址 0x100040000 超出范围
- atomic_store写入失败
- _cpus_lock包含垃圾值
- 后续spin_lock可能死循环

**验证方法**: 读取 ID_AA64MMFR0_EL1.PARange 确认默认PA位数

**假设2: 上下文切换调用链问题**

SMP版本的 rt_cpus_lock_status_restore 调用:
- rt_sched_post_ctx_switch(thread)
- rt_cpu_self() → 访问 _cpus[]
- 如果 _cpus[] 未正确初始化，可能触发异常

#### 与he200启动流程对比

| 项目 | he200 | HP232X BL22 |
|------|-------|--------------|
| 启动链 | bootwrapper → **u-boot SPL** → kernel | bootwrapper → kernel (无SPL) |
| MMU配置 | SPL帮忙配置TCR/MAIR/IPS | kernel自己配置 |
| EL降级 | SPL帮忙完成 | kernel自己做EL2→EL1 |
| SMP启动 | ✅ 8核正常 | ❌ 单核SMP就失败 |

**关键差异**: he200有u-boot SPL帮忙初始化，HP232X直接启动需要kernel自己做更多初始化。

#### 调试建议

**步骤调试输出** (components.c):
```c
rt_hw_spin_lock_init(&_cpus_lock);  early_putc_direct('1');
rt_hw_local_irq_disable();          early_putc_direct('2');
rt_hw_board_init();                 early_putc_direct('3');
rt_show_version();                  early_putc_direct('4');
rt_system_scheduler_init();         early_putc_direct('5');
rt_application_init();              early_putc_direct('6');
rt_thread_idle_init();              early_putc_direct('7');
rt_hw_spin_lock(&_cpus_lock);       early_putc_direct('8');
rt_system_scheduler_start();        early_putc_direct('9');
```

**上下文切换调试** (mp/context_gcc.S):
```asm
rt_hw_context_switch_to:
    early_putc 'A'
    ldr x0, [x0]
    mov sp, x0
    early_putc 'B'
    bl rt_cpus_lock_status_restore
    early_putc 'C'
    b _context_switch_exit
```

**IPS验证** (entry_point.S早期):
```asm
mrs x0, ID_AA64MMFR0_EL1
and x0, x0, #0xf   // PARange
// 打印x0值，确认是否 >= 5 (48-bit PA)
```

### 1. msh Shell 启动成功
提交：`40d77626df`

### 2. PMON 测试线程 Kconfig 控制
提交：`93effe12db` - 已禁用（msh 正常工作，不再需要 PMON 验证）

### 3. BL22 模式优化
提交：`8903c2d1eb`, `5d5a234ab6`
- 跳过 `pre_entry.S` 编译（bootwrapper 已完成 EL 降级）
- 移除未使用的栈定义（节省 ~6KB）
- `hp232x_spin_table` 移到 `.bss.noclean`（支持未来 SMP）

### 4. GIC 调试代码 Kconfig 控制
提交：`2e23182997`
- `RT_BSP_GIC_DBG` 默认关闭
- 节省代码空间

### 5. mm 组件移除尝试（失败）
文档：[doc/mm_component_removal_attempt.md](doc/mm_component_removal_attempt.md)
- 尝试移除 mm/*.c 节省 90KB，但导致启动失败
- 结论：保留 mm 组件，优先裁剪功能组件

---

## 当前内存使用（2026-07-06，SMP 双核配置）

```text
IRAM0 (File): 233.74 KB / 256 KB  (91.3%)  ← 瓶颈，剩 ~20 KB
IRAM1 (BSS):   69.61 KB / 256 KB  (27.2%)
Total:        303.35 KB

主要增量来源（相对 UP 单核 ~209KB IRAM0）：
  .text       +~25 KB   SMP 调度/上下文切换/双核 boot
  .mmu_table  +12 KB    24 KB（原 12 KB）
  IRAM1 BSS   +~18 KB   isr_table( INTERRUPT_INFO ) + SMP 结构
```

---

## SMP 双核 bring-up（已完成 2026-07-06）

原「下一步任务：多核启动」已完成。流程摘要：

```
bootwrapper WFE @ MBOX 0x401ff00
  → _secondary_cpu_entry (entry_point.S)
  → hp232x_mmu_secondary_init + GIC redist
  → rt_system_scheduler_start()
CPU0: rt_hw_secondary_cpu_up() 写 mbox + flush + SEV
```

文档：[doc/smp_irq_debug.md](doc/smp_irq_debug.md)（§8 双核、缓存一致性、串口互斥、1.5s delay）

~~以下 BL21 调试顺序建议已过时，BL22 双核已 PASS。~~

---

## Kconfig 配置项速查

| 配置项 | 当前值 | 说明 |
|--------|--------|------|
| `BSP_USING_HP232X_BL22` | y | BL22 启动模式 |
| `RT_BSP_PMON_TEST` | n | PMON 监控线程（已禁用） |
| `RT_BSP_GIC_DBG` | n | GIC 调试代码 |
| `BSP_USING_APB_TIMER_AS_TICK` | y | APB Timer 作为 tick |
| `RT_USING_MSH` | y | msh shell |
| `RT_CPUS_NR` | 2 | 双核 SMP（已验证） |
| `RT_USING_THREADSAFE_PRINTF` | y | SMP 串口互斥 |
| `RT_USING_INTERRUPT_INFO` | y | list_isr（+~8KB IRAM1） |
| `DBG_ENABLE` | y | LOG_X 宏可用 |

---

## 标准测试流程

```bash
cd /work/rt-thread/bsp/lynxi/hp232x
export RTT_CC_PREFIX=.../aarch64-none-elf-
scons -j8
python3 remote_test.py              # boot 抓串口
python3 test_msh_shell.py help      # msh 串口输入
python3 test_msh_shell.py list_isr  # IRQ 表
python3 test_msh_shell.py all       # 默认套件（各复位一次）
python3 test_multi.py               # up/smp/pmon 模式切换（改 rtconfig）
```

测试服务器：`192.168.49.81`，串口 `/dev/ttyUSB1`。

**SMP 注意**：`msh >` 出现后尽早发命令；CPU1 release 日志可能紧随其后，等 CPU1 ready 后再发命令易被串口并发输出打断。

---

## 相关文档

- [doc/smp_irq_debug.md](doc/smp_irq_debug.md) - SMP 单核/双核 + IRQ 调试
- [TEST_METHODOLOGY.md](TEST_METHODOLOGY.md) - 测试方法
- [doc/mm_component_removal_attempt.md](doc/mm_component_removal_attempt.md) - mm 移除尝试记录
- [doc/system_trim_plan.md](doc/system_trim_plan.md) - 系统裁剪计划
- [doc/mmu_design.md](doc/mmu_design.md) - MMU 设计

---

**更新日期**: 2026-07-06  
**当前状态**: SMP 双核 + msh + list_isr 实板 PASS；IRAM0 剩 ~20 KB  
**下一步**: IRAM0 内存裁剪（.text / .mmu_table），目标 ≤85% 占用