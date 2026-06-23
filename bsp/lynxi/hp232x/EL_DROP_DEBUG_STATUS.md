# HP232X EL降级调试进展

**日期**: 2026-06-23
**目标**: 从EL3降级到EL2/EL1，验证基础启动流程

---

## 调试进展总结

### ✅ 已完成

1. **镜像优化** (270KB → 228KB)
   - 禁用MMU页表定义（节省12KB）
   - 禁用RT_USING_DEBUG（节省30KB）
   - 总镜像大小：228KB（符合300KB限制）

2. **栈指针初始化修复**
   - 发现关键问题：调用`init_cpu_el`时栈指针未设置
   - 修复：使用IRAM0临时栈 `.hp232x_temp_stack_top`
   - 地址：0x04039000（IRAM0前256KB范围内）

3. **UART调试控制**
   - 统一宏控制：`BSP_USING_HP232X_DEBUG_UART`
   - entry_point.S的early_putc受DEBUG_UART控制
   - board.c的early_putc_direct受DEBUG_UART控制
   - 禁用RT_USING_DEBUG避免LOG_I输出

4. **异常处理简化**
   - vector_exception改为简单wfe循环（避免调用未实现的调试函数）
   - 定义`BSP_USING_HP232X_SPIN_TABLE`宏

### ❌ 当前问题

**核心问题**：启动早期触发异常，导致大量调试输出

**输出分析**：
```
ECO POK!           ← BL1 PCIe Boot成功
EEEXCC...          ← 混乱输出（EL降级标记 + 异常处理输出）
PC::00xx...        ← trap.c的rt_kprintf异常dump
```

**根本原因**：
- trap.c第155-166行：异常处理时输出完整寄存器dump
- `rt_kprintf`直接输出到UART，与早期调试标记混合
- UART输出本身可能触发异常

### 🔍 技术细节

**EL降级流程**（代码已添加）：
```
CurrentEL → '3' → 'P' → 'S' → 'P' → 'R' → '2' → 'V'
```

标记含义：
- `CurrentEL数字`: 启动时的异常级别（'2'或'3'）
- `3`: 在EL3
- `P`: Primary CPU检测
- `S`: SCR_EL3设置完成
- `P`: SPSR_EL3设置完成
- `R`: Ready to eret
- `2`: eret后到达EL2
- `V`: VBAR_EL2设置完成

**内存访问限制**（已确认）：
- IRAM0：0x04000000 ~ 0x0403FFFF（前256KB可访问）
- IRAM1：0x100040000 ~ 0x10007FFFF（后256KB可访问）
- 栈地址：0x04039000（IRAM0临时栈，正确）
- BSS段：0x100040000（IRAM1后256KB，正确）

---

## 文件修改清单

### rtconfig.h
```c
#define BSP_USING_HP232X
#define BSP_USING_HP232X_DEBUG_UART  // 启用UART调试
#define BSP_USING_HP232X_SPIN_TABLE   // 简化异常处理
/* #define RT_USING_DEBUG */          // 禁用LOG输出
```

### entry_point.S
- 第55-102行：early_putc宏定义（受DEBUG_UART控制）
- 第171-198行：CurrentEL输出（手动UART）
- 第206-255行：EL降级关键步骤标记
- 第268-269行：栈指针初始化（临时栈）

### board.c
- 第57-72行：early_putc_direct函数（受DEBUG_UART控制）
- 第191-401行：MMU配置代码（已用#if 0禁用）

---

## 下一步建议

### 方案1：禁用trap.c输出
暂时注释掉trap.c中的所有rt_kprintf，专注于EL降级本身

### 方案2：检查UART是否触发异常
- UART地址0x10006000是否正确？
- 是否需要UART初始化（时钟/电源）？
- BL1是否已正确初始化UART？

### 方案3：最小化测试
完全禁用UART输出，验证纯EL降级是否能成功
- 通过其他方式验证（如内存标记、GPIO等）

---

## 关键文件位置

| 文件 | 路径 | 关键内容 |
|------|------|---------|
| entry_point.S | libcpu/aarch64/cortex-a/entry_point.S | EL降级流程、栈初始化 |
| board.c | bsp/lynxi/hp232x/drivers/board.c | MMU配置（已禁用） |
| vector_gcc.S | libcpu/aarch64/common/vector_gcc.S | 异常向量表 |
| trap.c | libcpu/aarch64/common/trap.c | 异常处理、rt_kprintf输出 |
| rtconfig.h | bsp/lynxi/hp232x/rtconfig.h | 配置宏定义 |

---

## 相关文档

- [hp232x-memory-constraints.md](../../memory/hp232x-memory-constraints.md) - IRAM访问限制
- [hp232x-boot-debug-log.md](../../memory/hp232x-boot-debug-log.md) - 调试迭代记录
- [TEST_METHODOLOGY.md](TEST_METHODOLOGY.md) - 测试方法论