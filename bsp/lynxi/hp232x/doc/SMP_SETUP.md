# HP232X SMP 启动、跨核通信与 IRQ 调试

> 本文为 HP232X **SMP 总入口**：合并原 `SMP_COMM.md`（跨核通信）与 `smp_irq_debug.md`（bring-up / IRQ），并补充 **Flash 冷启多核定位**（2026-07-15）。后两份文件已删除，请只引用本文。
>
> 相关文档：[EMMC_TUNING.md](EMMC_TUNING.md)、[../BIZ_PORTING.md](../BIZ_PORTING.md)、[FLASH_PORTING.md](FLASH_PORTING.md)、[IMAGE_DESIGN.md](IMAGE_DESIGN.md)、[../Test_ENV.md](../Test_ENV.md)。

**更新日期**：2026-07-15

---

# Part A — Flash 冷启多核定位（2026-07-15）

UART 烧录与 Flash 冷启最终都进同一份 IRAM 镜像，但 **Flash 路径留下的硬件状态不同**，导致从核更容易在 MMU/early boot 阶段死亡。本节记录 2026-07-15 定位结论与已合入修复。

## A.1 UART 路径 vs Flash 路径

| 项 | UART（`test_uart.py` / xmodem） | Flash 冷启（hp640_bl jumper → RTT） |
|----|----------------------------------|-------------------------------------|
| 代码落点 | IRAM，拷完后同一 entry | 同上：`xip_memcpy` 后 jump IRAM entry |
| XIP 门控 `0x12600024` | 通常已非 `ba` / 或由前期打开 | jumper **留下 `ssi_ctrl=0xba…`（XIP 开）** |
| I/D cache | UART 链上已按正常 RTT boot 使能 | jumper 场景下 I/D cache 行为与 leave-XIP 顺序强相关 |
| CPU1 早期失败 | 相对少见（leave-XIP 不必先做） | 易在 `hp232x_mmu_secondary_init` 出 EXC |

**结论**：CPU1 挂死 **不是**「console / msh 时序太紧」一类假说。同一 IRAM 入口下，差异在 **Flash jumper 留下的 XIP（`ssi_ctrl=ba`）与 leave-XIP 时的 cache 维护**，以及 **共享 mbox + SEV 唤醒多颗停泊核**。

约束：勿改 hp640 jumper；RTT 对齐 `open_flash`（B8→98），细节见 [FLASH_PORTING.md](FLASH_PORTING.md)、[IMAGE_DESIGN.md](IMAGE_DESIGN.md)。

## A.2 失败 log 对照

### UART / 成功：`smp start`

期望 early 序列（可穿插 CPU0 的 `s` 与 wait 的 `.`）：

```text
sabcdeYI…
[SMP] CPU1 ready (idle=0x…)
```

含义：CPU0 SEV 完成（`s`）→ 从核 `a…e` → wait 见 idle（`Y`）→ 从核 idle 首入（`I`）。

### Flash 失败（修复前典型）

```text
aa…b…   # 多个 'a'，常到不了稳定的 'c'
EXC …   # 同步异常 / SError；无清晰 'c' 或随后卡死
```

- **无 `c`**：死在或死于 `hp232x_mmu_secondary_init()` 之前/之中（`b` 已持 `_cpus_lock`）。
- **EXC dump 字节级交错**：多核同时 `early_putc_direct`（无互斥）→ dump 穿插，像「乱码」，实为 **双核早期 UART**。

## A.3 根因与修复

### 1）leave-XIP 纯 invalidate 丢掉脏页表/数据

**现象**：Flash 冷启后 release CPU1 → `a`/`b` 后 EXC；UART 路径常能活过。

**根因**：leave-XIP 曾用 `__asm_invalidate_dcache_all`（仅 inv）。CPU0 脏的 MMU 页表、栈与数据行被丢弃，从核读 TTBR 指向的表为陈旧/零 → 死在 `hp232x_mmu_secondary_init`。

**修复**（对齐 hp640 `open_flash`）：

```c
/* drv_flash.c — flash_xip_leave_open_flash() */
__asm_flush_dcache_all();      /* clean+invalidate，保留脏写回 */
__asm_invalidate_icache_all();
/* 再写 B8 → settle → 98 + AHB pinmux */
```

release 前再刷页表：

```c
/* applications/main.c — msh smp start/release */
drv_flash_dbg_open_flash();
hp232x_mmu_flush_tables();   /* 再 flush L1/L2/L3 表到内存 */
rt_hw_secondary_cpu_up();
```

### 2）共享 mbox / SEV 唤醒多颗停泊核 → 多个 `a`

**现象**：连续多个 `a`，EXC 交错。

**根因**：bootwrapper 把多核 WFE 在自旋路径上；若各核轮询 **同一** mbox 槽（或 SEV 全局唤醒），写 CPU1 entry 会把 Aff1…AffN 一起拉进 `_secondary_cpu_entry`。

**修复（RTT 侧已做）**：`entry_point.S` 入口门禁 — 仅 `cpu_id ∈ [1, RT_CPUS_NR)` 继续，其余 `WFE` 死循环：

```asm
bl  rt_hw_cpu_id_set
bl  rt_hw_cpu_id
cbz x0, cpu_idle          /* CPU0 误入则闲 */
cmp x0, #(RT_CPUS_NR)
b.hs cpu_idle             /* id >= NR 则闲 */
```

**理想（bootwrapper）**：每核独立槽 `mbox + (linear_id - 1) * 8`，与 `HP232X_CPU_RELEASE_MBOX(cpu)` 一致，从源头避免多余核进入。

### 3）`THREADSAFE_PRINTF` / `_cpus_lock` 死锁与误判

从核在 `rt_hw_secondary_cpu_bsp_start()` 里：

1. `early_putc('a')` → prepare → **`rt_hw_spin_lock(&_cpus_lock)`**（`'b'`）→ MMU/GIC/`'e'` → `rt_system_scheduler_start()` 才释锁。

CPU0 若在此期间：

- `rt_kprintf` / `LOG_*`（`RT_USING_THREADSAFE_PRINTF` 要拿 console 相关锁，且调度路径可能碰 `_cpus_lock`）；
- 或 `rt_thread_mdelay()`（会调度），

则表现为 **释放成功但永不 `[SMP] CPU1 ready`**，甚至 banner 打到一半挂死。

**规则**：

| 阶段 | CPU0 | CPU1（从核） |
|------|------|----------------|
| 持 `_cpus_lock` 期间 | **禁止** `kprintf`/`mdelay`；只用 busy-wait + `early_putc` | **禁止** `kprintf`/`LOG`；只用 `early_putc` |
| `scheduler_start` 之后 | 可 `rt_kprintf` 打 ready | idle/`I` 后可走正常路径 |

配套宏与 CLI：

- `BSP_SMP_DEFER_SECONDARY`（`rtconfig.h`）：
  - **未定义（默认）**：`main` 自启动 — `leave-XIP` + `hp232x_mmu_flush_tables` + `rt_hw_secondary_cpu_up` + wait；
  - **定义**：命令行启动 — boot 不放核；msh `smp start` / `smp release`（同一套 leave-XIP+flush+release）；
- `components.c` 对 hp232x **始终**不调用 `rt_hw_secondary_cpu_up()`（避免 Flash 冷启在 leave-XIP 前放核）；
- wait：`hp232x_smp_wait_secondaries()`；带 `BSP_SMP_EARLY_MARK` 时才有 `early_putc` 进度符。

## A.4 Early 标记一览

| 字符 | 核 | 含义 |
|------|----|------|
| `a` | 从核 | `_secondary_cpu_bsp_start` 入口 |
| `b` | 从核 | 已拿 `_cpus_lock` |
| `c` | 从核 | `hp232x_mmu_secondary_init` 返回 |
| `d` | 从核 | GIC CPU/redist init 完成 |
| `e` | 从核 | 即将 `rt_system_scheduler_start` |
| `I` | 从核 | idle 首次进入（scheduler 已切成功） |
| `s` | CPU0 | `smp start` 里 SEV/release 循环之后 |
| `Y` / `N` | CPU0 | wait：online / timeout |
| `.` | CPU0 | wait 重试（周期性 rewrite mbox + SEV） |

成功串（示意）：`s` + `abcde` + `Y` + `I`（顺序可因双核交错略变，但应出现完整 `abcde` 与 `Y`）。

## A.5 推荐 CLI 顺序（冷启隔离）

当前 `rtconfig` 默认可开三宏做分步：`BSP_FLASH_DEFER_INIT`、`BSP_BIZ_SKIP_THREADS`、`BSP_SMP_DEFER_SECONDARY`。

```text
msh >
# 可选：Flash 步进（仅调试 SSI/JEDEC）
flash open_flash | flash init | flash worker

# 必做（双核）：leave-XIP + 刷表 + 放 CPU1
smp start

# 业务（绑 CPU1）
biz start [cpu]     # 默认 cpu=1
```

生产：关掉三个 DEFER/SKIP，自动 bring-up（仍建议 leave-XIP 发生在调度稳定之后，勿 `INIT_DEVICE` 里碰 SSI）。

## A.6 相关宏

| 宏 | 作用 | 建议 |
|----|------|------|
| `BSP_SMP_DEFER_SECONDARY` | 定义=msh 命令行启动；未定义=main 自启动 | 默认关（自启动）；隔离调试再开 |
| `BSP_FLASH_DEFER_INIT` | boot 不做 flash bring-up；msh `flash …` | 调试开 |
| `BSP_BIZ_SKIP_THREADS` | 不自动起 i2c/emmc_biz；msh `biz start` | 调试开 |
| `BSP_BOOT_EARLY_MARK` | 主核启动链 early_putc（P2I0 / IBKSUE / BOOT / EIMR / MMU 12345） | 默认关；冷启挂死定位再开 |
| `BSP_SMP_EARLY_MARK` | 从核/wait 的 early_putc（a..e / I / .YN） | 默认关；bring-up 再开 |
| `BSP_SMP_EXC_EARLY_DUMP` | 同步异常/SError 一次 early ESR dump | 默认关；挂死再开 |
| `RT_KERNEL_IRQ_DBG` | **每个** IRQ 刷 early UART | **勿开** |

## A.7 状态（2026-07-15）

| 项 | 状态 |
|----|------|
| Flash 冷启 CPU1（flush leave-XIP + `hp232x_mmu_flush_tables` + entry 门禁 + DEFER/`smp start`） | ✅ |
| 纯 inv leave-XIP | ❌ 已废弃 |
| 1.5s `mdelay` 后再 release | ❌ 已删除（反会因 `_cpus_lock` 更危险） |

---

# Part B — 跨核通信

HP232X 当前为 **SMP 双核**（`RT_CPUS_NR=2`）。多核「通信」在本 BSP 中主要指：

| 机制 | 用途 | 典型场景 |
|------|------|----------|
| **spin-table mbox** | CPU0 释放 CPU1 进内核 | 上电 / `rt_hw_secondary_cpu_up()` / msh `smp start` |
| **GIC SGI (IPI)** | 跨核调度唤醒 | `RT_SCHEDULE_IPI`、`RT_SMP_CALL_IPI` |
| **共享内存 + cache flush** | 跨核可见的调度/线程数据 | CPU0 创建线程绑定 CPU1 |
| **WFE / SEV** | 低功耗等待与唤醒 | CPU1 idle、`hp232x_kick_cpu()` |

**设计目标**：CPU0 跑 I2C/msh/finsh；CPU1 跑 `emmc_biz` 轮询主循环，减少与 tuning 时序相关的调度干扰。

---

## B.1 端到端流程总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│ CPU0 (Boot CPU)                                                         │
├─────────────────────────────────────────────────────────────────────────┤
│ rtthread_startup() → rt_hw_board_init() → rt_components_init()          │
│   → [若无 BSP_SMP_DEFER_SECONDARY] rt_hw_secondary_cpu_up()             │
│   → main()                                                              │
│       → [若 BSP_SMP_DEFER_SECONDARY] 提示 msh: smp start                  │
│       → [否则] hp232x_smp_wait_secondaries()                            │
│       → biz_emmc_biz_start()  或  msh: biz start [cpu]                  │
│            ├ rt_thread_create                                           │
│            ├ RT_THREAD_CTRL_BIND_CPU → CPU1                             │
│            ├ invalidate mpidr + arm_gic_sgi_affinity_reset()            │
│            ├ rt_thread_startup()    # 在 CPU0 上下文执行                │
│            └ hp232x_kick_cpu(1)     # flush + SCHEDULE IPI + SEV        │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                    mbox @ 0x401fff0 │  IPI (SGI)
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ CPU1 (Secondary)                                                        │
├─────────────────────────────────────────────────────────────────────────┤
│ bootwrapper WFE → _secondary_cpu_entry（门禁 cpu_id）                    │
│   → rt_hw_secondary_cpu_bsp_start  # a b c d e                          │
│   → GIC + rt_system_scheduler_start()  # tidle1；I                      │
│ rt_hw_secondary_cpu_idle_exec():                                        │
│   rt_schedule(); rt_hw_wfe();                                           │
│ 收到 SCHEDULE IPI:                                                      │
│   rt_scheduler_ipi_handler → rt_schedule() → irq_switch_flag=1          │
│ IRQ 退出: rt_scheduler_do_irq_switch → rt_hw_context_switch_interrupt   │
│   → emmc_biz / 绑定线程体开始执行                                       │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## B.2 阶段一：从核 Boot（spin-table）

### B.2.1 硬件约定

| 项 | 值 / 说明 |
|----|-----------|
| CPU1 mbox 物理地址 | `0x401fff0`（`HP232X_CPU_RELEASE_MBOX(1)`，线性 ID 1 → 偏移 **+0**） |
| 入口符号 | `_secondary_cpu_entry`（`entry_point.S`） |
| bootwrapper | WFE 轮询 mbox；理想每核 `(id-1)*8`；见 Part A.3.2 |

### B.2.2 CPU0 释放流程（`board.c`）

`rt_hw_secondary_cpu_up()` / `hp232x_smp_release_cpu()`：

1. 对各从核 mbox 先写 `0`，**dcache flush**；
2. `dsb sy`；
3. 写 `entry = (uint64_t)_secondary_cpu_entry`，再 **flush** + `rt_hw_sev()`（实现里对每核写两次，防 WFE 与首 SEV 竞态）；
4. **禁止**在 release 前后对持锁中的从核路径使用 `mdelay`/`kprintf`（见 Part A.3.3）。旧版 **~1.5s delay 已删除**。

**必须 flush**：mbox 位于 IRAM0 Normal WB；CPU0 写可能仅在 cache，CPU1 读不到则永不启动。

### B.2.3 从核启动路径

```
_secondary_cpu_entry
  → [hp232x] gate: cpu_id in [1, RT_CPUS_NR) else WFE
  → init_cpu_el (EL2→EL1)
  → init_cpu_stack_early（IRAM1 栈，MMU 关）
  → rt_hw_secondary_cpu_bsp_start()
       → 'a' prepare → lock 'b'
       → hp232x_mmu_secondary_init() → 'c'
       → arm_gic_cpu_init + arm_gic_redist_init → 'd'
       → unmask IPI → 'e' → rt_system_scheduler_start()
```

### B.2.4 release 时序与 DEFER

`components.c` → `main_thread_entry`：

```c
#if defined(BSP_USING_HP232X) && defined(BSP_SMP_DEFER_SECONDARY)
    /* 跳过：等 msh smp start */
#else
    rt_hw_secondary_cpu_up();
#endif
    main();
```

| 模式 | 行为 |
|------|------|
| 默认生产（无 DEFER） | components 内 release → `main` 里 `hp232x_smp_wait_secondaries()` |
| `BSP_SMP_DEFER_SECONDARY` | boot 单核到 `msh >`；再 `smp start`（先 leave-XIP + `hp232x_mmu_flush_tables`） |

Flash 冷启推荐 DEFER 路径，见 Part A.5。

---

## B.3 阶段二：CPU1 idle 就绪

`hp232x_smp_wait_secondaries()`（`main.c`）：busy-wait，**不** `mdelay`/`kprintf` 直到 online：

```c
while (retry-- > 0) {
    invalidate struct rt_cpu;
    if (current_thread != NULL) break;
    /* 周期 rewrite mbox + early_putc('.') */
    rt_hw_us_delay(10);
}
/* 'Y' 后可 rt_kprintf("[SMP] CPU%u ready …") */
```

成功标志：

```text
Y
[SMP] CPU1 ready (idle=0x........)
```

表示 CPU1 已完成 `rt_system_scheduler_start()`，`tidle1` 为当前线程。

---

## B.4 阶段三：跨核线程创建与调度

实现位于 `biz/biz_subsys.c`（`biz_emmc_biz_start_on_cpu`），CLI 在 `biz/biz_finsh_cmds.c`（`biz start [cpu]`）。

### B.4.1 标准启动序列（CPU0 创建，CPU1 运行）

```c
t = rt_thread_create("emmc_biz", entry, ...);
rt_thread_control(t, RT_THREAD_CTRL_BIND_CPU, (void *)(rt_ubase_t)cpu);
/* cpu != 0: invalidate mpidr 表 + arm_gic_sgi_affinity_reset() */
rt_thread_startup(t);
/* cpu != 0: flush TCB/stack + hp232x_kick_cpu(cpu) */
```

### B.4.2 内核侧跨核 insert（`scheduler_mp.c`）

当 **创建核 ≠ 绑定核** 时，insert 后必须：

```c
rt_hw_cpu_dcache_ops(FLUSH, rt_cpu_index(0), sizeof(struct rt_cpu) * RT_CPUS_NR);
rt_hw_cpu_dcache_ops(FLUSH, thread, sizeof(*thread));
rt_hw_cpu_dcache_ops(FLUSH, thread->stack_addr, thread->stack_size);
rt_hw_barrier(dsb, sy);
rt_hw_ipi_send(RT_SCHEDULE_IPI, 1 << bind_cpu);
```

否则 CPU1 可能看到空 ready queue，或 context switch 后栈/TCB 不一致。

### B.4.3 CPU1 idle 循环（`board.c`）

```c
void rt_hw_secondary_cpu_idle_exec(void)
{
    /* 首次：early_putc('I') */
    rt_schedule();      // 非 IRQ 上下文，可直接切换
    rt_hw_wfe();
}
```

**不要在 idle 或 IPI handler 中对 `rt_cpu` 做 invalidate**（Inner Shareable 域应依赖 snoop；invalidate 会破坏 CPU1 看到的 `priority_group`）。

### B.4.4 IPI 调度路径（IRQ 延迟切换）

```
vector_irq
  → rt_interrupt_enter          # irq_nest++
  → rt_hw_trap_irq → IPI ISR
       → rt_scheduler_ipi_handler → rt_schedule()
            # irq_nest>0 → 仅置 irq_switch_flag=1
  → rt_interrupt_leave          # irq_nest--
  → rt_hw_vector_irq_sched
       → rt_scheduler_do_irq_switch
            # irq_nest==0 && irq_switch_flag → rt_hw_context_switch_interrupt
  → rt_hw_irq_exit → eret
```

`irq_switch_flag=1` 出现在 IPI handler **之后**、实际 switch **之前** 是正常现象。IRQ 延迟切换寄存器问题见 Part C §C.5.6。

---

## B.5 缓存一致性与 MPIDR

### B.5.1 必须 flush 的对象

| 对象 | 时机 | 原因 |
|------|------|------|
| spin-table mbox | CPU0 release 从核 | 从核 MMU 开前按物理地址读 |
| leave-XIP 后页表 | `hp232x_mmu_flush_tables` | Flash leave-XIP 的 dcache 操作后，CPU1 TTBR 可见 |
| `struct rt_cpu[]` | 跨核 insert 线程后 | ready queue / priority_group |
| `struct rt_thread` | 同上 | TCB、sp 指针 |
| 线程栈 | 同上 | `rt_hw_stack_init()` 在 CPU0 cache 中 |

### B.5.2 禁止 / 慎用

| 操作 | 后果 |
|------|------|
| CPU1 上对 `rt_cpu` **invalidate** | `priority_group` 读成垃圾（如 `0x80000000`） |
| 运行时改写 `rt_cpu_mpidr_table[]` 为 raw MPIDR | GIC SGI affinity 路由错误；**保持静态 `0x0` / `0x1`**（从核 prepare 会写自身项并 flush） |
| 跨核 insert 后不 flush 就发 IPI | CPU1 看不到新线程 |
| leave-XIP 只用 invalidate_dcache_all | 脏页表丢失 → Part A.3.1 |

### B.5.3 MPIDR 与 SGI

```c
// board — 表初值；从核 prepare 读 mpidr_el1 写入本核槽并 flush
rt_uint64_t rt_cpu_mpidr_table[] = { [0] = 0x0, [1] = 0x1 };
```

`biz_subsys.c` 绑到非 0 核前：

1. invalidate `rt_cpu_mpidr_table`；
2. `arm_gic_sgi_affinity_reset()`；
3. `hp232x_kick_cpu()` 前 flush 目标核 `struct rt_cpu`。

---

## B.6 rtconfig 宏与运行模式

| 宏 | 作用 | 生产建议 |
|----|------|----------|
| `RT_USING_SMP` / `RT_CPUS_NR 2` | 双核 | 保持 |
| `RT_USING_STDC_ATOMIC` | timer ISR 内原子锁 | **必需** |
| `RT_USING_HEAP_ISR` | SMP 堆 spinlock | **必需** |
| `RT_USING_THREADSAFE_PRINTF` | 多核 `rt_kprintf` 互斥 | **必需**（从核 boot 仍勿用之） |
| `BSP_BIZ_EMMC_ON_CPU1` | 自动 start 时 `emmc_biz` 绑 CPU1 | 启用 |
| `BSP_SMP_DEFER_SECONDARY` 等 | 见 Part A.6 | 调试开 / 生产关 |
| `BSP_USING_HP232X_SPIN_TABLE` | 简化 spin 路径 | **勿开**（破坏 IRQ） |

### B.6.1 模式切换

**手动隔离（当前调试默认）**：

```c
#define BSP_FLASH_DEFER_INIT
#define BSP_BIZ_SKIP_THREADS
#define BSP_SMP_DEFER_SECONDARY
#define BSP_BIZ_EMMC_ON_CPU1
```

msh：`smp start` → `biz start 1`。

**生产自动**：

```c
/* #define BSP_FLASH_DEFER_INIT */
/* #define BSP_BIZ_SKIP_THREADS */
/* #define BSP_SMP_DEFER_SECONDARY */
#define BSP_BIZ_EMMC_ON_CPU1
```

---

## B.7 当前状态（2026-07-15）

| 项 | 状态 |
|----|------|
| CPU1 boot + idle（UART） | ✅ `[SMP] CPU1 ready` |
| **Flash 冷启 CPU1** | ✅ flush leave-XIP + 门禁 + DEFER/`smp start`（2026-07-15） |
| IPI 到达 CPU1 | ✅ |
| CPU1 ready queue 可见 | ✅ flush 后 `list_empty=0` |
| `emmc_biz` @ CPU1 | ✅ 见 [EMMC_TUNING.md](EMMC_TUNING.md)、[../BIZ_PORTING.md](../BIZ_PORTING.md) |
| 旧「cpu1_test 无 entry」怀疑 | 已被 biz 路径与 flush/kick 路径覆盖；上下文 fix 见 Part C §C.5.6 |

诊断：

- 从核路径只用 `early_putc`（`a`…`e`/`I`）；
- 对比 `libcpu/aarch64/common/mp/context_gcc.S` 中 `rt_hw_context_switch_interrupt`（x23–x25 fix）。

---

## B.8 测试方法

### B.8.1 测试环境

| 用途 | 主机 | 串口 | 烧录方式 |
|------|------|------|----------|
| **eMMC / 业务 / SMP / Flash 冷启（当前）** | 192.168.58.36（lynxi/lx@123） | `/dev/ttyUSB0` @ 115200 | xmodem / Host 升级 |
| SMP msh / 启动链（历史） | 192.168.49.81（lynxi/1） | `/dev/ttyUSB1` | PCIe boot `remote_test.py` |

详见 [../Test_ENV.md](../Test_ENV.md)。

### B.8.2 编译

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
scons -j$(nproc)
# 产物: rtthread-header.bin
```

### B.8.3 板测 58.36 — 软链 + 烧录 + 抓 log

**1）切换固件软链**（测试机）：

```bash
cd /home/lynxi/xia/xmodem
ln -sf /mnt/49.20/lynxlink/lynxi-rtt/bsp/lynxi/hp232x/rtthread-header.bin u-boot-spl.bin
```

**2）推荐 `flash_and_log.py`（烧录后连续读串口）**：

```bash
LOG=/tmp/hp232x_smp_$(date +%s).log
/usr/local/lynx/tools/mcu-tools -l 0 -t 1 -i 2 reset_mcu
sleep 3
cd /home/lynxi/xia/xmodem/scripts && sudo python3 flash_and_log.py
grep -aE 'SMP|cpu1|IPI|emmc_biz|sabcde|Y|EXC|leave-XIP' "$LOG"
```

**3）Flash 冷启**：按 [IMAGE_DESIGN.md](IMAGE_DESIGN.md) 组包写 Flash，复位后 msh：

```text
smp start
biz start 1
```

期待 early：`s`/`abcde`/`Y`/`I`，再有 `[SMP] CPU1 ready` / `[drv] emmc_biz entry CPU1`。

### B.8.4 SMP 判据

| 阶段 | 成功 log |
|------|----------|
| leave-XIP | `[drv] flash leave-XIP ok ssi_ctrl=0x98000000` |
| 从核 boot | early `abcde`；无交错 EXC |
| idle 就绪 | `Y` + `[SMP] CPU1 ready` |
| emmc_biz | `[drv] emmc_biz entry CPU1` + tuning/heartbeat |

### B.8.5 开发机远程脚本（49.81）

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
python3 remote_test.py
python3 test_msh_shell.py help
python3 test_msh_shell.py list_isr
python3 test_multi.py smp_msh
```

### B.8.6 A/B：hp640 SPL 对比

```bash
cd /home/lynxi/xia/xmodem
ln -sf /mnt/49.20/lynxlink/output/uboot/spl/u-boot-spl-hp640-header.bin u-boot-spl.bin
grep -aE 'Heart-beat|tuning|tap=' /tmp/board.log
```

---

## B.9 常见故障排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 无 `[SMP] CPU1 ready`，有 `a`/`b` 无 `c` | leave-XIP inv-only / 页表未 flush | Part A.3.1：flush + `hp232x_mmu_flush_tables` |
| 多个 `a`、EXC 交错 | 多核进同一 entry | Part A.3.2：门禁；理想 bootwrapper 分槽 |
| release OK 但 ready 永不出现 / banner 半截挂 | 等核时 `kprintf`/`mdelay` | Part A.3.3：busy-wait + early_putc |
| 无 ready，无 early | mbox 未 flush / 地址错 | 查 `rt_hw_secondary_cpu_up()` |
| IPI 无响应 | GIC redist 未 init / MPIDR | 查从核 GIC；MPIDR 表 |
| `pg=0x80000000` | CPU1 invalidate `rt_cpu` | 去掉 invalidate |
| `list_empty=1` 但有 bind 线程 | 跨核 insert 未 flush | `scheduler_mp.c` flush 三件套 |
| 串口 log 交错 | 多核 printf | 保持 `THREADSAFE_PRINTF`；boot 仍用 early |
| 一敲键盘 Data abort @ 0x5 | context_switch_interrupt x20 | Part C §C.5.6（已 fix） |

---

## B.10 关键源文件

| 文件 | 职责 |
|------|------|
| `drivers/board.c` | mbox release、从核 BSP、kick、idle exec、`early_putc` |
| `biz/biz_subsys.c` | 线程 bind/start、`hp232x_kick_cpu`、`emmc_biz` |
| `biz/biz_finsh_cmds.c` | `biz` / `flash` msh |
| `applications/main.c` | wait、`smp` msh、defer 提示 |
| `drivers/drv_flash.c` | leave-XIP flush（`flash_xip_leave_open_flash`） |
| `drivers/hp232x_mmu.c` | `hp232x_mmu_flush_tables` / secondary init |
| `libcpu/.../entry_point.S` | `_secondary_cpu_entry` 门禁 |
| `src/components.c` | `BSP_SMP_DEFER_SECONDARY` 跳过 up |
| `src/scheduler_mp.c` | 跨核 insert + flush + IPI |
| `libcpu/.../gicv3.c` | `arm_gic_sgi_affinity_reset()` |
| `libcpu/.../mp/context_gcc.S` | IRQ 延迟 context switch |
| `rtconfig.h` | SMP / DEFER / 业务宏 |

---

## B.11 相关文档

| 文档 | 内容 |
|------|------|
| **本文 Part A / C** | Flash 冷启；IRQ / 分层 bring-up |
| [EMMC_TUNING.md](EMMC_TUNING.md) | HS200 tuning / heartbeat |
| [../BIZ_PORTING.md](../BIZ_PORTING.md) | hp640→hp232x 业务移植 |
| [FLASH_PORTING.md](FLASH_PORTING.md) | SSI / leave-XIP / Host 升级 |
| [IMAGE_DESIGN.md](IMAGE_DESIGN.md) | BL 链 / jumper / 组包 |
| [../Test_ENV.md](../Test_ENV.md) | 58.36 测试机、烧录脚本 |
| [../TEST_METHODOLOGY.md](../TEST_METHODOLOGY.md) | 49.81 自动化 |
| [../HANDOFF_SLIM.md](../HANDOFF_SLIM.md) | BSP/IRAM 约束 |

（原独立 `SMP_COMM.md` / `smp_irq_debug.md` 内容已并入本文；请以 **SMP_SETUP.md** 为准。）

---

# Part C — SMP bring-up 与 IRQ 调试

合并原 `smp_irq_debug.md`：SMP 单核/双核分层 bring-up，以及 AArch64 IRQ/异常定位。适用：

- UP → SMP 单核（`RT_CPUS_NR=1`）→ 多核；
- SMP 能到 `msh >` 但串口无响应或 Data abort；
- IRQ 不进入、ISR 卡死、IRQ 返回跳飞、BSS 未清零；
- Flash 冷启从核问题优先查 **Part A**，再回到本节 IRQ 基础设施。

其它：调度器历史 [scheduler_debug_summary.md](scheduler_debug_summary.md)；MMU/BSS [mmu_design.md](mmu_design.md)。

---

## C.1 推荐策略：先 UP，再 SMP 单核

| 阶段 | rtconfig 要点 | 验证目标 |
|------|---------------|----------|
| UP | 注释 `RT_USING_SMP`，保留 `RT_CPUS_NR 1` | tick、UART、BSS、MMU 正常 |
| SMP 单核 | `#define RT_USING_SMP`，`RT_CPUS_NR 1` | MP 调度、IRQ 延迟切换、finsh |
| SMP 多核 | `RT_CPUS_NR 2+` | secondary、IPI、spin table；Flash 见 Part A |

**原则**：UP 已证明硬件无问题时，SMP 故障优先查 **内核 MP 路径** 与 **rtconfig**，勿反复改 BSP boot。Flash 冷启另计 leave-XIP/cache。

### C.1.1 UP 下 tick 验证（pmon）

```c
/* #define RT_USING_SMP */
#define RT_CPUS_NR 1
/* #define RT_USING_MSH */
/* #define RT_USING_FINSH */
#define RT_BSP_PMON_TEST
```

`pmon_gic.c` 每 500ms 打印 tick/isr；100Hz 下预期各 **+50**。APB timer 计数见 `hp232x_apb_timer_isr_count`。

---

## C.2 SMP 单核最小 rtconfig（对齐 he200 要点）

```c
#define RT_USING_SMP
#define RT_CPUS_NR 1

#define RT_USING_STDC_ATOMIC   /* 避免 timer ISR 里 rt_cpus_lock 死锁 */
#define RT_USING_HEAP_ISR      /* SMP 堆 spinlock；finsh rt_calloc */

#define RT_USING_MSH
#define RT_USING_FINSH
```

| 项 | he200 | hp232x |
|----|-------|--------|
| Tick | `BSP_USING_CORETIMER` | `BSP_USING_APB_TIMER_AS_TICK` |
| 堆 | SLAB | `RT_USING_SMALL_MEM` 48KB IRAM |
| `RT_USING_TIMER_SOFT` | 开 | 关（省 IRAM） |
| `RT_CPUS_NR` | 8 | 先 1 再 2 |

**不要** 再引入 hp232x 专用 kernel hook（`components.c` / `stack.c` / `shell.c` 的 `BSP_USING_HP232X` 延迟 finsh 等已移除）。

---

## C.3 排查思路（分层）

```
Layer 0: 硬件 / 早期 boot
  entry_point.S BSS、门禁、hp232x_mmu、board、Flash leave-XIP（Part A）

Layer 1: UP 内核路径
  无 RT_USING_SMP → tick / msh / 串口输入

Layer 2: SMP 配置宏
  STDC_ATOMIC + HEAP_ISR → 能否 boot、finsh 是否卡死

Layer 3: SMP 调度 / 上下文切换
  rt_system_scheduler_start、rt_hw_context_switch_to

Layer 4: IRQ 延迟切换
  tick / UART RX → rt_hw_context_switch_interrupt

Layer 5: 应用 / shell
  finsh / biz / smp start
```

| 现象 | 优先查 |
|------|--------|
| Flash 冷启有 `a`/`b` 无 `c` / EXC | Part A；Layer 0 |
| 卡在 scheduler_start / 无 `msh` | Layer 0–3；§C.11–C.13 |
| 有 `msh >`，输入无反应 | Layer 4–5；§C.5.4、§C.5.6 |
| 一按键 Data abort @ `0x5` | Layer 4；§C.5.6 |
| `rt_calloc` / finsh 卡死 | `RT_USING_HEAP_ISR` |
| timer 不进 ISR | BSS、APB timer、GIC；§C.14、§C.15 |
| 第一次 IRQ 后 PC 跳 BSS | §C.14 |

---

## C.4 调试手段（SMP）

### C.4.1 编译与上板

```bash
export RTT_CC_PREFIX=.../aarch64-none-elf-
cd bsp/lynxi/hp232x && scons -j8
python3 remote_test.py
python3 test_msh_shell.py help
python3 test_msh_shell.py all
```

测试服务器：`192.168.49.81`，串口 `/dev/ttyUSB1`。业务板见 Part B.8（58.36）。

### C.4.2 配置切换

`test_multi.py`：`up_msh` / `smp_msh` / `smp_pmon` 等预设改 `rtconfig.h`。

### C.4.3 崩溃定位

```bash
addr2line -e rtthread.elf -a -f <EPC>
```

| PC 区域 | 含义 |
|---------|------|
| `mp/context_gcc.S` `rt_hw_context_switch_to` | 首次切到首个线程 |
| `mp/context_gcc.S` `rt_hw_context_switch_interrupt` | IRQ 退出延迟切换 |
| `scheduler_mp.c` | 调度器 |
| `finsh/cmd.c` | 常为二次崩溃，查 primary fault |
| `hp232x_mmu_secondary_init` | Flash 冷启 Part A |

逐步打印见 §C.10–C.14。从核 early 标记见 Part A.4（优先于 `RT_KERNEL_IRQ_DBG`）。

---

## C.5 已验证问题与 fix（2026-07）

### C.5.1 rtconfig 宏（SMP 必需）

| 问题 | 现象 | Fix |
|------|------|-----|
| 缺 `RT_USING_STDC_ATOMIC` | timer ISR 内 `rt_cpus_lock` 死锁 | `#define RT_USING_STDC_ATOMIC` |
| 缺 `RT_USING_HEAP_ISR` | finsh `rt_calloc` 在 `rt_mutex_take` 挂死 | `#define RT_USING_HEAP_ISR` |

### C.5.2 `scheduler_mp.c`：过早解锁 scheduler lock

`rt_sched_post_ctx_switch()` 在 `from_thread` 存在时无条件 `SCHEDULER_CONTEXT_UNLOCK`，首次 `rt_hw_context_switch_to` 时 `from_thread==NULL` 也会误解锁 → `_mp_scheduler_lock` 损坏。

**Fix**：仅当 `from_thread && SCHEDULER_LOCK_FLAG(pcpu)` 时解锁。

### C.5.3 `trap.c`：SMP 链接 UP-only 符号

SMP 构建不应引用 `rt_thread_switch_interrupt_flag` 等 UP 变量。

**Fix**：`#ifndef RT_USING_SMP` 包裹 UP debug 符号与引用。

### C.5.4 `shell.c`：finsh 未 startup（hp232x hack 残留）

曾跳过 `rt_thread_startup(tid)` 依赖延迟启动 → tshell 不运行。

**Fix**：vanilla — `finsh_system_init()` 内始终 `rt_thread_startup(tid)`。

### C.5.5 应移除的 hp232x kernel hack（勿恢复）

| 文件 | 错误做法 | 原因 |
|------|----------|------|
| `components.c` | `hp232x_smp_main_irq_*`、延迟 finsh | he200 无此逻辑 |
| `stack.c` | 全局 SPSR I-mask | finsh IRQ 关 → UART RX 死 |
| `mp/context_gcc.S` | `_context_switch_exit` 里 `DAIFSet` | 非 upstream |
| `stack_gcc.S` | `_thread_start` I-cache 维护 | 非必需 |

`BSP_SMP_DEFER_SECONDARY` 仅跳过 `rt_hw_secondary_cpu_up()`，不是上述 hack。

### C.5.6 【根因】`rt_hw_context_switch_interrupt` 寄存器与宏冲突

**现象**：SMP 到 `msh >`，**一敲键盘** Data abort，`fault addr = 0x5`，`X20 = 0x5`，PC 在 `str x0, [FROM_SPP]`。

**原因**：

- `rt_hw_context_switch_interrupt` 用 **x20** 存 `FROM_SPP`；
- `SAVE_CONTEXT_SWITCH_FAST` 宏内 **`mov x20, #0x05`**（EL1h SPSR）覆盖 x20 → 写到地址 0x5。

**Fix**（`libcpu/aarch64/common/mp/context_gcc.S`）：

```asm
#define EXP_FRAME   x19
#define FROM_SPP    x23    /* 原 x20，避开宏 */
#define TO_SPP      x24
#define TO_TCB      x25
```

- `rt_hw_context_switch_to` / 线程内 switch **不经**该宏 → boot/`msh` 提示符正常；
- UART RX / tick 触发 **IRQ 退出延迟切换** 才爆炸（fix 前）。

对所有 aarch64 SMP 有效，建议合入 upstream。

---

## C.6 最终验证清单

| 项 | 命令 / 现象 | 状态 |
|----|-------------|------|
| UP + pmon tick | 每 500ms +50 tick/isr | ✅ |
| SMP boot | `Hi, this is RT-Thread!` | ✅ |
| SMP msh | `msh >` + `help` | ✅ |
| 无 Data abort @ 0x5 | 按键后稳定 | ✅（§C.5.6） |
| SMP 双核 `RT_CPUS_NR=2` | UART：CPU1 ready + msh | ✅ |
| Flash 冷启 CPU1 | `smp start` → `abcdeY` | ✅（Part A，2026-07-15） |

---

## C.7 涉及文件索引

| 类型 | 路径 |
|------|------|
| BSP 配置 | [rtconfig.h](../rtconfig.h) |
| BSP boot/MMU/GIC/IPI | [board.c](../drivers/board.c)、[entry_point.S](../../../libcpu/aarch64/cortex-a/entry_point.S)、[hp232x_mmu.c](../drivers/hp232x_mmu.c) |
| Flash leave-XIP | [drv_flash.c](../drivers/drv_flash.c) |
| 内核 fix | [scheduler_mp.c](../../../src/scheduler_mp.c)、[trap.c](../../../libcpu/aarch64/common/trap.c)、[components.c](../../../src/components.c) |
| 上下文切换 fix | [mp/context_gcc.S](../../../libcpu/aarch64/common/mp/context_gcc.S) |
| 业务 / CLI | [biz_subsys.c](../biz/biz_subsys.c)、[main.c](../applications/main.c) |
| 测试 | [remote_test.py](../remote_test.py)、[test_msh_shell.py](../test_msh_shell.py)、[pmon_gic.c](../applications/pmon_gic.c) |

---

## C.8 SMP 双核（RT_CPUS_NR=2）已验证要点

实板（49.81 UART；58.36 业务/Flash）在 `RT_CPUS_NR=2` 下：

- CPU0 → `msh >`，串口输入正常；
- CPU1 release：`mbox=0x401fff0`；early `abcde` / `[SMP] CPU1 ready`；
- Flash 冷启：须 leave-XIP **flush** + 页表 flush + entry 门禁（Part A）。

### C.8.1 rtconfig 补充（相对 §C.2）

```c
#define RT_CPUS_NR 2
#define DBG_ENABLE
#define RT_USING_THREADSAFE_PRINTF
#define RT_USING_MUTEX
/* 调试: BSP_SMP_DEFER_SECONDARY + BSP_SMP_EXC_EARLY_DUMP */
/* 勿: BSP_USING_HP232X_SPIN_TABLE、RT_KERNEL_IRQ_DBG */
```

### C.8.2 spin-table 与多核缓存一致性

BL22 从核在 bootwrapper `spin.S` 里 WFE 轮询 **`0x401fff0 + (id-1)*8`**（与 [board.h](../drivers/board.h) 一致）。若 bootwrapper 实际共享单槽，依赖 RTT `entry_point` 门禁（Part A.3.2）。

| 项 | 说明 |
|----|------|
| mbox 基址 | **`0x401fff0`**（旧误 `0x401ff00`） |
| CPU1 偏移 | **+0**；勿写成 `MBOX+0x8` |
| **必须 dcache flush** | mbox Normal WB |
| 屏障 | FLUSH + `dsb sy` + SEV |

实现：`board.c` `rt_hw_secondary_cpu_up()` / `hp232x_smp_release_cpu()`。

### C.8.3 从核启动路径（与 MMU）

见 Part B.2.3。要点：

- MMU 关时可用物理地址访 IRAM1；
- 从核须配齐 MAIR/TCR/TTBR；页表由 CPU0 建好并 flush。

### C.8.4 串口互斥与 LOG_X

**LOG 不打印**：`LOG_I/D/E` 依赖 `DBG_ENABLE`（或 `RT_USING_DEBUG`）。

**SMP 串口**：`RT_USING_THREADSAFE_PRINTF` 保证单次 `rt_kprintf` 原子。

| 路径 | 是否互斥 |
|------|----------|
| `rt_kprintf` / `rt_kputs` | ✅（开 THREADSAFE 后） |
| `LOG_I` 一条 | ⚠️ 内部多次 `rt_kprintf`，行内仍可交错 |
| `early_putc_direct` | ❌ 直写 MMIO；**从核 boot / wait 必用此** |

从核在 `_cpus_lock` 下 **禁止** `rt_kprintf`/`LOG_*`（Part A.3.3）。

### C.8.5 release 策略（更新 2026-07-15）

~~`rt_hw_secondary_cpu_up()` 开头 `rt_thread_mdelay(1500)`~~ — **已删除**。

原因：

1. 从核持 `_cpus_lock` 时，CPU0 `mdelay`→调度 / `kprintf` 会与 `THREADSAFE_PRINTF` **死锁**；
2. 固定 sleep 不能替代正确的 leave-XIP / cache 维护。

**现行**：

| 手段 | 说明 |
|------|------|
| `BSP_SMP_DEFER_SECONDARY` | boot 单核到 msh，再 `smp start` |
| leave-XIP flush + `hp232x_mmu_flush_tables` | Flash 冷启 **在** release 前做完 |
| `hp232x_smp_wait_secondaries` | busy-wait + `early_putc`，禁止等核时 `kprintf`/`mdelay` |

### C.8.6 测试

```bash
cd bsp/lynxi/hp232x && scons -j8
python3 test_msh_shell.py help
python3 test_msh_shell.py all
# Flash / 业务板：msh smp start；见 Part A.5、B.8
```

---

## C.9 （后续）

1. 扩核时递增 `cpu_release_paddr[]` / bootwrapper mbox 步长（每核 +8）；
2. bootwrapper 分槽消除多余 `a`（Part A.3.2 理想方案）；
3. 多核下重复 `test_msh_shell.py all` 与 tick/pmon。

---

# Part C（续）— IRQ 与异常故障定位

与 SMP bring-up 共用 early UART。典型案例：第一次 IRQ 后 PC 跳 BSS → hook 垃圾 → `blr`；见 §C.15。

---

## C.10 调试开关

### C.10.1 `RT_KERNEL_IRQ_DBG`（慎用）

[libcpu/aarch64/Kconfig](../../../libcpu/aarch64/Kconfig)：打开后 **每个** IRQ/exception 刷 early UART。

- **仅** 深度 bring-up；常态必须关（洪水、改时序）。
- SMP Flash/从核问题优先 `BSP_SMP_EXC_EARLY_DUMP` + Part A early 标记，**不要**开 `RT_KERNEL_IRQ_DBG`。

```c
/* #define RT_KERNEL_IRQ_DBG */
#define BSP_SMP_EXC_EARLY_DUMP   /* 一次 ESR dump，推荐 */
```

### C.10.2 打印原则

IRQ/异常早期 **不建议** `rt_kprintf()`（console/heap/锁未稳；可能二次故障）。用：

- `board.c`：`early_putc_direct()`；
- `trap.c`：`early_puts_direct()` / `early_puthex64()`。

汇编 `bl` 调试函数须保存 **x0–x18、x30**。

---

## C.11 IRQ 主路径打印点（`RT_KERNEL_IRQ_DBG`）

入口：[vector_gcc.S](../../../libcpu/aarch64/common/vector_gcc.S)

```text
IRQ entry
IRQ step=0x0000000000000001
...
IRQ get_irq ret=0x000000000000001e
IRQ handler=0x... irq=0x...
IRQ handler done
IRQ ack done
IRQ trap exit
IRQ step=0x0000000000000006
IRQ step=0x0000000000000007
IRQ exit: ELR=0x... SPSR=0x... SP=0x...
```

### C.11.1 step 含义

| 输出 | 位置 | 含义 |
|------|------|------|
| `IRQ entry` | `vector_irq` | 进入 IRQ vector |
| `IRQ step=1` | `rt_interrupt_enter()` 前 | 栈帧已存 |
| `IRQ step=2` | enter 后 | 嵌套计数正常 |
| `IRQ step=3` | `SAVE_USER_CTX` 后 | 准备 C 分发 |
| `IRQ trap enter` | `rt_hw_trap_irq()` | C trap |
| `IRQ get_irq ret=` | GIC IRQ ID | active IRQ |
| `IRQ handler=` | 调 ISR 前 | handler + 号 |
| `IRQ handler done` | ISR 返回 | ISR 未卡死 |
| `IRQ ack done` | EOI 后 | ACK 完成 |
| `IRQ step=6` | leave 后 | 嵌套恢复 |
| `IRQ step=7` | `rt_hw_vector_irq_sched` 后 | SMP 尾部调度返回 |
| `IRQ exit` | `RESTORE_IRQ_CONTEXT` 前 | 将 `eret` |

### C.11.2 按 step 判断

| 停在哪 | 排查 |
|--------|------|
| 仅 `IRQ entry` | `SAVE_IRQ_CONTEXT`、FPU |
| `step=1` 无 `step=2` | `rt_interrupt_enter`、hook、**BSS**（§C.14） |
| `step=3` 无 `get_irq` | GIC SRE、ICC |
| 有 `handler=` 无 `done` | ISR 死循环/异常 |
| 有 `done` 无 `ack` | EOI |
| `IRQ exit` 后异常 | ELR/SPSR、栈帧（§C.13） |

---

## C.12 异常路径打印点

```text
EXC trap entry
EXC detail: ESR=… EC=… FAR=… ELR=… SPSR=…
EXC regs: PC=… CPSR=… X0=… X30=…
```

| 字段 | 含义 |
|------|------|
| `ESR` / `EC` | `0x24/0x25` data abort |
| `FAR` | fault 地址 |
| `ELR` | 异常返回 PC；落在 BSS/heap 则异常 |
| `X30` | LR |

Flash 冷启从核 EXC 常伴 early `a`/`b` 无 `c` — 先 Part A，再 `addr2line` ELR。

```bash
cd bsp/lynxi/hp232x
aarch64-none-elf-nm -n rtthread.elf | grep -E '10005c|rt_interrupt|mmu_secondary'
```

---

## C.13 IRQ 尾部调度与上下文切换

### C.13.1 SMP 路径

`vector_irq` 末尾 → `rt_hw_vector_irq_sched` → `rt_scheduler_do_irq_switch()` → 必要时 `rt_hw_context_switch_interrupt`（[mp/context_gcc.S](../../../libcpu/aarch64/common/mp/context_gcc.S)）。

UP：`rt_thread_switch_interrupt_flag` + `rt_hw_context_switch_interrupt_do`（up 路径）；SMP 不走。

### C.13.2 观察重点

1. `from`/`to` 是否为 TCB 中 `sp` 字段的**地址**；
2. `new_sp` 是否在线程栈内；
3. 栈帧 `ELR/SPSR` 合法；
4. **`FROM_SPP` 不可与 `SAVE_CONTEXT_SWITCH_FAST` 共用 x20**（§C.5.6）。

---

## C.14 案例：IRQ 后 PC 跳 BSS `0x10005c07c`

### C.14.1 现象 / 日志

```text
IRQ entry
IRQ step=0x0000000000000001
EXC detail: ... ELR=0x000000010005c07c ... X30=0x000000000405a818
```

停在 `step=1`↔`2` → `rt_interrupt_enter()`；`X30` 指向 hook 返回 → `rt_interrupt_enter_hook` 垃圾 → `blr` 跳 BSS。

### C.14.2 根因与修复

`entry_point.S` 对高地址 BSS 曾跳过 early 清零。当前 `init_kernel_bss` 已清 BSS；或在 `rt_hw_board_init()` 最早处 `rt_memset(bss)`，须在 hook/`earlycon`/调度全局使用**之前**。

修复后 IRQ 能走完 step 1→7，ELR 回 `.text`。

---

## C.15 常见故障模式速查

| 现象 | 排查 |
|------|------|
| 无 `IRQ entry` | DAIF.I、VBAR、GIC、Group1NS、HCR_EL2.IMO |
| 仅到 `step=1` | enter、hook、**BSS** |
| ISR 不返回 | ISR 内 `rt_kprintf`、heap、timer reload |
| 第一次 IRQ 后不再触发 | timer reload、EOI、DAIF/SPSR |
| IRQ 返回跳飞 | SAVE/RESTORE、FPU、调试保存、切换栈帧 |
| SMP 按键 @ `0x5` | §C.5.6 |
| Flash 冷启多 `a` / 无 `c` | Part A |
| release 后 shell 挂死 | Part A.3.3（`_cpus_lock`） |

---

## C.16 调试结束后的清理

```c
/* #define RT_KERNEL_IRQ_DBG */
/* 可保留: #define BSP_SMP_EXC_EARLY_DUMP */
```

```bash
cd bsp/lynxi/hp232x && scons -j8 && python3 remote_test.py
```

长期开 `RT_KERNEL_IRQ_DBG` 会掩盖竞态；串口输入回归在**关闭**该宏下测。

---

## C.17 变更记录

| 日期 | 内容 |
|------|------|
| 2026-07-15 | 合并为 `SMP_SETUP.md`（删原 `SMP_COMM.md` / `smp_irq_debug.md`）；Part A Flash 冷启（flush leave-XIP、门禁、DEFER/`smp start`）；`biz_subsys`；状态更新 Flash CPU1 OK |
| 2026-07-13 | 跨核通信、bind、测试环境（原 `SMP_COMM.md`） |
| 2026-07-06 | SMP 单核 bring-up；IRQ 定位；双核 spin-table flush、THREADSAFE_PRINTF（原 `smp_irq_debug.md`） |
