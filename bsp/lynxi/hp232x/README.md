# HP232X BSP for KA200 (IRAM-only, direct boot)

## Current Status (2026-06-25)

**🎉 系统启动成功！UART架构重构完成**

Latest Update:
- ✅ Complete EL transition: EL3 → EL2 → EL1
- ✅ MMU页表配置成功（PGD/PUD/PMD在IRAM0，12KB）
- ✅ MMU启用成功：Identity mapping工作
- ✅ IRAM1访问：通过MMU访问4GB边界外地址
- ✅ **Small memory allocator成功**：malloc/free工作
- ✅ **GICv3基础实现**：Distributor + Redistributor + ICC系统寄存器
- ✅ **UART架构重构完成**：统一early_putc宏，去掉DEBUG_UART不异常
- ✅ Kernel启动：Banner完整显示
- ✅ 内存约束满足：IRAM0 80%利用率，IRAM1 52%利用率

**编译结果**:
- IRAM0 (File): 204.63 KB / 256 KB (79.9%)
- IRAM1 (BSS): 132.00 KB / 256 KB (51.6%)
- Total Memory: 336.63 KB
- Boot Image: 212 KB (带 header)

## Overview

This BSP targets the **HP232X** board using the **KA200** SoC — an ARMv8-A
dual-core processor with **two IRAM segments** and **no external DDR**.

| Feature       | Value                                    |
|---------------|------------------------------------------|
| SoC           | KA200 (ARMv8-A, Cortex-A53 class)       |
| CPU cores     | 2 (dual-core SMP)                        |
| Memory        | IRAM0 512KB + IRAM1 512KB (no DDR)      |
| Boot flow     | BL1 → pre_entry.S → entry_point.S → RT-Thread |
| UART          | DW APB UART0 @ 0x10006000, 115200 baud  |
| GIC           | GICv3 / GIC500                           |

## Memory Layout

**HP232X Memory Constraints**:
- IRAM0: 前256KB可用 (0x04000000-0x0403FFFF)
- IRAM0: 后256KB保留 (0x04040000-0x0407FFFF)
- IRAM1: 前256KB保留 (0x100000000-0x10003FFFF)
- IRAM1: 后256KB可用 (0x100040000-0x10007FFFF)

**Section Analysis**:
```
IRAM0 (204.63 KB):
  .head: 启动入口 (0.66 KB)
  .text: 内核代码 + 驱动 (180.23 KB)
  .early_hp232x: Bootwrapper栈 + spin table (8 KB)
  .data: 已初始化全局变量 (3.64 KB)
  .mmu_table: MMU页表 (12 KB)

IRAM1 (132.00 KB):
  BSS section (144 KB)
  Heap预留 (48 KB)
  Page pool (16 KB)
  其他预留 (线程栈、内核数据)
```

## Boot Flow

```
BL1 Boot ROM @ IRAM0 0x04000000
  ↓ 读取32B header
跳转到 _start @ 0x04000020
  ↓
bl hp232x_bootwrapper_init (drivers/pre_entry.S)
  ↓ EL3初始化
[EL3] Primary CPU:
  - CCI500 Interconnect初始化
  - CPUECTLR.SMPEn设置
  - SCR_EL3配置 (NS=1, RW=1)
  - GICv3 ICC_SRE_EL3启用
  - GIC Distributor配置 (GICD_CTLR=0x37)
  - GIC Redistributor配置
  - SPI中断组别设置 (Group 1 NS)
  - ICC系统寄存器配置
  - EL3 → EL2 → EL1降级
  ↓
[EL1] init_cpu_el (entry_point.S)
  - SCTLR_EL1, CPACR_EL1配置
  ↓
init_kernel_bss
  ↓
init_cpu_stack_early
  ↓
init_mmu_early + enable_mmu_early
  ↓
rtthread_startup → Shell启动
```

## Building

```bash
cd bsp/lynxi/hp232x
rm -rf build && scons -j$(nproc)
python3 mkimage.py rtthread.bin rtthread-header.bin
```

**生成文件**:
- rtthread.elf: ELF可执行文件 (990 KB)
- rtthread.bin: 纯二进制镜像 (212 KB)
- rtthread-header.bin: 带Lynxi header启动镜像 (212 KB)

## Testing

```bash
python3 remote_test.py
```

**预期输出**:
```
ECO
POK!
???E3SCGIDXNARBMN234NSPE12MCIBK
[DEBUG] GICD_CTLR: 0x12

 \ | /
- RT -     Thread Operating System
 / | \     5.3.0 build Jun 25 2026 15:50:23
```

## Debug UART Output

启用`BSP_USING_HP232X_DEBUG_UART`后，启动时会输出调试标记：

```
???E3SCGIDXNARBMN234NSPE12MCIBK
  E3: CurrentEL=3确认
  SC: SCR_EL3配置前后
  GID: GIC Distributor初始化
  XNARBM: GIC配置标记
  234N: Redistributor唤醒流程
  SPE12MC: ICC系统寄存器配置
  IBK: entry_point.S标记
```

## Documentation

### 核心知识点文档 (doc/)
- [GIC调试要点](doc/gic_debug.md) - GICv3中断控制器配置
- [MMU调试要点](doc/mmu_debug.md) - 页表配置和MMU启用
- [EL降级要点](doc/el_transition.md) - EL3→EL2→EL1降级流程
- [汇编调试技巧](doc/asm_debug_tips.md) - UART调试、寄存器追踪
- [内存分配器](doc/memory_allocator.md) - Small mem vs SLAB选择

### 交接文档
- [HANDOFF_SLIM.md](HANDOFF_SLIM.md) - 项目进展和下一步任务
- [TEST_METHODOLOGY.md](TEST_METHODOLOGY.md) - 自动化测试方法

## Key Configurations

```c
// rtconfig.h
#define BSP_USING_HP232X               // 启用HP232X平台
#define BSP_USING_HP232X_DEBUG_UART    // UART调试输出
#define BSP_USING_GICV3                // GICv3支持
#define RT_CPUS_NR 2                   // 双核SMP
#define ARCH_HEAP_SIZE 0x0C000         // 48KB heap
#define ARCH_INIT_PAGE_SIZE 0x04000    // 16KB page pool
#define RT_USING_SMALL_MEM             // Small mem allocator（适配144KB heap）
```

## Code Architecture

**Platform-specific Bootwrapper** ([drivers/pre_entry.S](drivers/pre_entry.S)):
- EL3初始化（bootwrapper风格）
- GICv3 Distributor + Redistributor + ICC配置
- EL降级流程（EL3 → EL2 → EL1）

**Common RT-Thread Initialization** ([entry_point.S](../../../libcpu/aarch64/cortex-a/entry_point.S)):
- `_start`入口点
- `bl hp232x_bootwrapper_init` - 调用平台初始化
- `init_cpu_el` - 通用EL初始化
- `init_kernel_bss` - BSS清零
- `init_mmu_early` - MMU启用
- `rtthread_startup` - 内核启动