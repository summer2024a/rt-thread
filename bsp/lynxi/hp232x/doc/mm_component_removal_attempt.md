# HP232X MM 组件移除尝试 (2026-07-03)

## 背景

hp232x 使用自定义 MMU 实现 (`hp232x_mmu_init`)，不需要 RT-Thread 标准 mm 框架（`mm_aspace`, `mm_page` 等）。尝试移除 mm 组件以减少内存占用。

## 修改内容

### 1. Kconfig 修改
- 移除 `KA200_SOC` 选择 `ARCH_ARM_MMU`
- 避免自动选择 `ARCH_MM_MMU`

### 2. rtconfig.h 修改
- 移除 `ARCH_MM_MMU`, `ARCH_ARM_MMU`, `ARCH_USING_ASID`
- 添加 `ARCH_PAGE_SHIFT`, `ARCH_PAGE_SIZE` 定义（汇编需要）

### 3. 本地头文件创建
- `drivers/mmu.h` - 提供 `struct mem_desc`, `ARCH_PAGE_SIZE` 等基础定义
- 避免依赖 libcpu 的 mmu.h（它会 include mm_aspace.h）

### 4. libcpu 修改
- `libcpu/aarch64/common/SConscript` - 排除 `backtrace.c`, `mmu.c`, `trap.c`, `cpu_spin_table.c`
- `libcpu/aarch64/common/include/cpu.h` - 条件性 include mm_aspace.h
- `libcpu/aarch64/common/interrupt.c` - 条件性 include ioremap.h
- `libcpu/aarch64/cortex-a/entry_point.S` - 条件性编译 `init_mmu_early`, `enable_mmu_early`

### 5. 新增文件
- `drivers/trap_stub.c` - 提供 `rt_hw_trap_irq`, `rt_hw_trap_exception` 等 stub 函数

## 内存减少效果

| 区域 | 之前 | 之后 | 减少 |
|------|------|------|------|
| IRAM0 (.text) | 216.77 KB | 169.02 KB | -47.75 KB |
| IRAM1 (.bss) | 51.78 KB | 9.76 KB | -42.02 KB |
| **总计** | 268.55 KB | 178.78 KB | **-89.77 KB** |

## 发现的问题

**症状**: 系统在显示 RT-Thread banner 后停止输出，未进入 shell。

**输出截断点**:
```
ECO POK!
IBKSUE
BOOT
IRAM1: bss@100051000-100052708 page@100053000-100057000 heap@100057000-10005f000
[DEBUG] ===== Verifying GIC configuration =====
  GICD_CTLR: 0x12 (ARE_NS view at EL1 NS)
========================================

 \ | /
- RT -     Thread Operating System
 / | \     5.3.0 build Jul  3 2026 12:26:59
 2006 - 2024 Copyright by RT-Thread team

（之后无输出）
```

**可能原因**:
1. **trap 处理缺失**: `trap_stub.c` 的 stub 函数可能无法正确处理实际异常/中断
2. **interrupt.c 依赖**: GIC 中断初始化可能依赖 mm 函数（虽然代码路径看似不依赖）
3. **其他 libcpu 依赖**: 排除的文件可能包含必要的初始化代码

## 结论

**⚠️ 本次裁剪尝试失败** - 移除 mm 组件导致系统无法正常启动。

**建议**: 
- 不要轻易移除 ARCH_MM_MMU，即使 hp232x 使用自定义 MMU
- libcpu 代码与 mm 框架有隐性依赖，需要更深入分析
- 如需节省内存，优先考虑禁用功能组件而非架构组件

## 后续方向

如果未来需要再次尝试移除 mm 组件：
1. 需要更详细分析 libcpu 的隐性依赖
2. 可能需要保留 trap.c 的核心功能
3. 需要验证 interrupt.c 在无 mm 时的完整功能

当前建议：**保留 mm 组件**，保持系统稳定运行。

## 相关文档

- [memory_layout_comparison.md](memory_layout_comparison.md) - 内存布局对比
- [mmu_design.md](mmu_design.md) - MMU 设计文档
- [system_trim_plan.md](system_trim_plan.md) - 系统裁剪计划