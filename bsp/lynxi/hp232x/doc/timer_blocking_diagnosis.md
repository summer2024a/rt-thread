# HP232X Arch Timer 诊断与 PLL 时钟分析

## 文档信息

| 项 | 内容 |
|---|---|
| 初版 | 2026-06-27（Timer 中断阻塞排查） |
| 更新 | 2026-07-13（PLL 时钟流程、CNTPCT 实测、根因收敛） |
| 状态 | **arch timer 频率问题已解决**；GIC PPI30 默认 enable 仍待 gicv3 正式配置 |

---

## 1. 问题背景

HP232X BL22 启动链为 **bootwrapper(EVB) → RT-Thread**，无 U-Boot SPL。早期现象：

- `BSP_USING_CORETIMER` 时 tick / `mdelay` 行为异常
- `gtimer.c` 中为 HP232X 硬编码 `timer_step = 500000`（假设 CNTPCT 只有 500 kHz）
- 2026-06 诊断认为「GIC/Timer 软件配置均正确，但 PPI30 中断不触发」

he200 同 SoC Lite CPR，启动链为 **bootwrapper → SPL → RT-Thread**，arch timer 正常。

---

## 2. 启动链对比

| 阶段 | he200 | hp232x BL22 |
|------|-------|-------------|
| EL3 bootwrapper | `0x08600000` 平台 timer + `cntfrq_el0=31.25M` | 同左 |
| SPL | `pll_init()` + DDR init + … | **跳过** |
| RT-Thread sysctl | `lynxi_sysctl_lite_init()`（fabric/UART gate） | 需自行补齐 |
| 系统 tick | 各核 gtimer (PPI30) | 曾用 APB timer workaround |

SPL 中与 arch timer **最相关**的代码只有 `pll_init()`，不含 `0x08600000` 写（该部分在 bootwrapper EL3 完成）。

```c
/* lynxi-uboot/common/spl/spl.c → board_init_f() */
pll_init();
ddr_pll_reset();
lynxi_ddrfirmware_read();
lynxi_sdram_init();
lynxi_uboot_read();
```

---

## 3. PLL 时钟流程（核心）

### 3.1 CPR 关键寄存器

| 寄存器 | 地址 (CPR base `0x12500000`) | 作用 |
|--------|------------------------------|------|
| `BOOT_SELECT` | +`0x64` | 多位 mux：**选 PLL 还是 24MHz 晶振**；高位 `[31:28]` 为启动介质 |
| `PLL_LOCK_STATUS` | +`0xd4` | PLL lock 状态，EVB 期望低 16 bit = `0x3f3f` |
| CPU timer gate | +`0x68` bit8 | Linux `LITE_TIMER` / `cpu_timer` 门控 |
| Fabric gate | +`0x6c` bit5/9 | `fabric_aclk6` / `fabric_pclk2` |

参考：`lynxi-uboot/common/lx_common/lx_boot.c`、`lynxi-linux/drivers/clk/lynxi/clk-lynxi-lite.c`。

### 3.2 `pll_init()` 两步逻辑

```c
/* lynxi-uboot/common/lx_common/lx_boot.c */
while ((readl(PLL_LOCK_STATUS) & 0xffff) != 0x3f3f)
    ;   /* ① 等所有 PLL lock */

writel(readl(BOOT_SELECT) & 0xffffffd0, BOOT_SELECT);  /* ② 切到 PLL 时钟 */
```

**① 等 PLL lock**

上电后 CPU/外设通常先跑 **24MHz 参考时钟（xin24m）**；BootROM 启动各 PLL（CPU 1.5G、Fabric 800M、DDR 等）。在 lock 完成前切换 mux 会导致时钟不稳定。

**② `BOOT_SELECT &= 0xffffffd0`**

`0xffffffd0` 清零 **bit 0、1、2、3、5**，保留 bit4 及 `[31:28]` boot mode。

Linux 时钟树中 mux 语义（以 bit0 为例）：

```
cpu_free [CPR+0x64 bit0]:
  0 → cpu_high  → CPU PLL 1.5GHz 路径
  1 → xin24m    → 24MHz 晶振路径
```

大量 fabric/emmc 外设共用 **bit1** 等 mux 位，规则相同：**0 = PLL 派生，1 = 24M 参考**。

因此这条语句的语义是：

> **在 PLL 已稳定后，把 CPU / Fabric / DDR 等时钟域从 24MHz 安全时钟切换到 PLL 输出。**

不是「打开某个单一 PLL」，而是 **全局 mux 切源**。

### 3.3 Arch Timer 时钟树

```
xin24m (24MHz) ──┐
                 ├─ cpu_free [0x64 bit0] ─ cpu_div6 (/6) ─ timer_div8 (/8) ─ cpu_timer [0x68 bit8] ─ CNTPCT
cpu PLL (1.5G) ──┘   (via cpu_high)
```

| 路径 | 计算 | CNTPCT 频率 |
|------|------|-------------|
| PLL | 1500M / 6 / 8 | **31.25 MHz** = `CNTFRQ_EL0` |
| 24M ref | 24M / 6 / 8 | **500 kHz** |

`CNTFRQ_EL0` 由 bootwrapper EL3 写入 `31250000`；**CNTPCT 实际速率由 CPR mux 决定**，与 `CNTFRQ` 寄存器值可以不一致。

### 3.4 各 EL 职责（arch timer 相关）

| EL | 组件 | 做什么 |
|----|------|--------|
| EL3 | bootwrapper `boot.S` | 写 `0x08600000` 平台 timer；`msr cntfrq_el0` |
| EL3/SPL | `pll_init()` | 等 lock → `BOOT_SELECT` 切 PLL |
| EL2 | `entry_point.S` | `CNTHCTL_EL2`、清 HCR TTC/TSC/TPC |
| EL1 | RT-Thread | CNTP 编程、GIC PPI30、gtimer tick |

---

## 4. 曾误判「500 kHz」的原因

### 4.1 错误假设

`libcpu/aarch64/common/gtimer.c` 曾写入：

```c
#ifdef BSP_USING_HP232X
    timer_step = 500000;  /* 假设 CNTPCT 只有 500kHz */
#else
    timer_step = rt_hw_get_gtimer_frq();
#endif
```

推断链：若 `cpu_free` mux 仍在 24M 路径 → CNTPCT = 500 kHz，而 `CNTFRQ=31.25M` → tick 周期错 62.5 倍 → `mdelay`/调度异常。

### 4.2 为何推断不成立（实板 2026-07-13）

仅开 fabric gate（`CPR+0x6c` bit5/9）时，sysctl 前后 CNTPCT delta 不变——说明 **问题不在 fabric gate，而在 mux 是否走 PLL**。

但进一步 wall-clock 实测表明：**BL22 路径上 CNTPCT 本来就是 31.25 MHz**。

| 测量方法 | 结果 | 说明 |
|----------|------|------|
| 2M nop 窗口 delta（sysctl 前/后） | ~291667 / ~291667 | 对 sysctl 不敏感，不能单独证明频率 |
| **APB timer1 wall 100ms** | **3125002 ticks** | **≈ 31.25 MHz，与 CNTFRQ 一致** |

Wall probe 实现要点（`board.c`，`BSP_USING_HP232X_ARCH_TIMER_PROBE`）：

- 用 DesignWare APB timer1 @ 50MHz 作时间基准
- **free-running 模式**（仅 `ENABLE`，不设 `MODE` bit），避免 periodic 重载导致多计周期
- 100ms 内 CNTPCT delta 期望 ~3125000（PLL 路径）或 ~50000（24M 路径）

实板寄存器快照：

```
BOOT_SELECT:  0x9000001d → 0x90000010  (sysctl pll 切换后)
CPR+0xd4:     0x00003f3f              (PLL 已 lock)
CPR+0x68:     0x000007ff              (含 cpu_timer bit8)
CNTFRQ:       31250000 Hz
wall 100ms:   CNTPCT delta = 3125002
```

**结论**：bootrom/bootwrapper 已 lock PLL 且 CNTPCT 在 31.25MHz 运行；SPL `pll_init()` 在 BL22 上是 **补齐/确认**，不是从 0 起切 PLL。`timer_step=500000` workaround **掩盖了真实问题**。

---

## 5. 解决 arch timer 的核心问题

### 5.1 根因（2026-07 收敛）

| 层级 | 真实问题 | 非根因 |
|------|----------|--------|
| 时钟频率 | CNTPCT **已是 31.25MHz** | ~~CNTPCT 只有 500kHz~~ |
| tick 配置 | `gtimer.c` 硬编码 500k 导致 tick 慢 62.5 倍 | ~~需 SPL pll_init 才能到 31.25M~~（BL22 上 bootrom 已完成） |
| 中断路径 | GIC 初始化时 **PPI30 默认未 enable**（`GICR_ISENABLER0 bit30=0`） | 向量表 `b vector_irq` 是正常跳转 |
| SMP | APB tick 模式下 CPU1 误开 `gtimer_local_enable` 会占 PPI30 | 已在 `board.c` 修复 |

### 5.2 已实施修复

1. **删除 `gtimer.c` 500kHz workaround**  
   统一使用 `timer_step = rt_hw_get_gtimer_frq()`（31.25M / `RT_TICK_PER_SECOND`）。

2. **恢复 `BSP_USING_CORETIMER`**  
   实板验证：`rt_hw_gtimer_init` OK，`msh >` 正常，SMP CPU1 bind OK。

3. **补齐 sysctl（`drivers/clock/drv_sysctl_lite.c`）**  
   - SPL 等效 `_sysctl_pll_boot_select()`：`PLL_LOCK` → `BOOT_SELECT &= 0xffffffd0`  
   - 开 `CPR+0x68 bit8`（cpu_timer gate）  
   - fabric / UART / WDT / GPIO gate（对齐 he200）

4. **Arch timer probe（可选，`BSP_USING_HP232X_ARCH_TIMER_PROBE`）**  
   启动时打印 CPR 寄存器、nop delta、wall 100ms delta，便于回归。

### 5.3 已改进：PPI30 在 gicv3 正式 enable

`arm_gic_redist_init()` 会先 `ICENABLER0=0xffffffff` 关掉全部 PPI/SGI。原先 board.c 在 `rt_hw_gtimer_init()` 之前检查 bit30=0 并手动写 GICR——这是**时序误判**（此时尚未 unmask）+ **board 补丁**。

现已在 `libcpu/aarch64/common/gicv3.c` 中，当 `BSP_USING_CORETIMER` 时 redist 初始化末尾 enable PPI30（priority 已在同函数设为 0xa0）：

```c
#if defined(BSP_USING_CORETIMER)
    GIC_RDISTSGI_ISENABLER0(redist_base) = (1U << ARM_ARCH_TIMER_PPI); /* 30 */
    arm_gicv3_wait_rwp(0, ARM_ARCH_TIMER_PPI);
#endif
```

APB tick 模式（未定义 `BSP_USING_CORETIMER`）不 enable PPI30，与 SMP 从核跳过 gtimer 的策略一致。

board.c 中手动 enable + timer 自测块已删除。

---

## 6. BL22 补齐 SPL 等效代码

```c
/* drivers/clock/drv_sysctl_lite.c — 对齐 lynxi-uboot pll_init() 核心 */

#define CPR_BOOT_SELECT_OFF      0x64u
#define CPR_PLL_LOCK_STATUS_OFF  0xd4u
#define CPR_PLL_LOCK_VALUE       0x3f3f3u
#define CPR_BOOT_SELECT_PLL_MASK 0xffffffd0u

static void _sysctl_pll_boot_select(void)
{
    while ((SYSCTL_REG32(CPR_PLL_LOCK_STATUS_OFF) & 0xffff) != CPR_PLL_LOCK_VALUE)
        ;

    v = SYSCTL_REG32(CPR_BOOT_SELECT_OFF);
    v &= CPR_BOOT_SELECT_PLL_MASK;
    SYSCTL_REG32(CPR_BOOT_SELECT_OFF) = v;
    dsb sy;
}

void lynxi_sysctl_lite_init(void)
{
    _sysctl_pll_boot_select();
    _sysctl_gate_on(0x68, 8);   /* cpu_timer */
    _sysctl_gate_on(0x6c, 5);   /* fabric_aclk6 */
    _sysctl_gate_on(0x6c, 9);   /* fabric_pclk2 */
    /* UART / WDT / GPIO ... */
}
```

Kconfig：`BSP_USING_SYSCTL_CLK`（默认建议开启）。

---

## 7. 2026-06 原始诊断摘要（存档）

早期认为「软件全对但 Timer 不触发」，逐项排除了 DAIF、GICD/GICR、CNTP_CTL、ISR 安装等。其中：

- **vector_irq[0]=0x14000215** 是 `b vector_irq` 正常跳转，非死循环（除非开启 `BSP_USING_HP232X_SPIN_TABLE`）。
- **timer_isr_counter=0** 更可能与 PPI30 未 enable + 错误 tick 分频叠加有关，而非向量表本身损坏。

完整原始检查清单见 git 历史；当前以第 5 节为准。

---

## 8. 验证清单

实板（192.168.49.81，`python3 remote_test.py`）：

- [x] `[arch-timer] wall 100ms CNTPCT delta ≈ 3125000`
- [x] `[arch-timer] wall CNTPCT rate ≈ 31250000 Hz`
- [x] `BSP_USING_CORETIMER` → `rt_hw_gtimer_init ok`
- [x] `msh >` 出现
- [x] SMP CPU1 `GIC/IPI ready, enter scheduler`
- [x] GIC redist init enable PPI30 when `BSP_USING_CORETIMER`（`gicv3.c`，无需 board 补丁）
- [ ] `mdelay(1000)` 与 wall clock 对齐（CORETIMER 下抽样）

---

## 9. 相关文件

| 文件 | 说明 |
|------|------|
| `drivers/clock/drv_sysctl_lite.c` | SPL 等效 pll + gate |
| `drivers/board.c` | sysctl 调用、arch timer probe、PPI30 临时补丁 |
| `libcpu/aarch64/common/gtimer.c` | 已移除 HP232X 500k workaround |
| `lynxi-uboot/common/lx_common/lx_boot.c` | SPL `pll_init()` 参考 |
| `lynxi-linux/drivers/clk/lynxi/clk-lynxi-lite.c` | 完整 Lite CPR 时钟树 |
| `lynxi-bootwrapper/boot.S` | EL3 `0x08600000` + `cntfrq` |
| `rtconfig.h` | `BSP_USING_CORETIMER`、`BSP_USING_SYSCTL_CLK` |

---

**最后更新**: 2026-07-13
