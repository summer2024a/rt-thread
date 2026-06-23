# HP232X EL降级调试总结 (2026-06-23 最终状态)

## 调试目标

严格按照bootwrapper实现EL降级，满足内存约束（IRAM0前256KB + IRAM1后256KB）。

## ✅ 已完成的工作

### 1. 内存规划实施
- **IRAM0使用**: 194KB（满足前256KB约束）
- **IRAM1使用**: .bss在正确位置（后256KB）
- **段布局**: 所有段在约束范围内

### 2. Bootwrapper流程完整实现

严格按照`/work/lynxi-bootwrapper/boot.S`和`spin.S`实现：

**EL3初始化** (boot.S:34-113):
- ✅ 检查CurrentEL
- ✅ 检查主核（MPIDR）
- ✅ 设置SCR_EL3 (RES1位 + NS=1 + HVC=1 + RW=1)
- ✅ 禁用CPTR_EL3
- ✅ 设置cntfrq_el0 (31250000Hz)
- ✅ 设置CPUECTLR.SMPEn（如果CPU支持）

**EL3→EL2降级** (spin.S:18-26):
- ✅ 设置SCTLR_EL2_RESET
- ✅ 设置ELR_EL3和SPSR_EL3
- ✅ 执行eret

**EL2启动** (spin.S:28-68):
- ✅ 清零x0-x3寄存器
- ✅ 再次检查MPIDR
- ✅ 次核spin等待
- ✅ 主核跳转到kernel

### 3. 关键修改

**移除不必要代码**:
- ✅ 移除看门狗关闭（bootwrapper默认关闭）
- ✅ 移除setup_stack（不调用interconnect_init/gic_secure_init）
- ✅ 移除UART调试输出（完全禁用）

**保持bootwrapper风格**:
- ✅ 不设置异常向量表（bootwrapper也没有）
- ✅ 直接跳转到kernel（不经过中间层）

## ⚠️ 当前问题

### 启动异常症状

```
ECO POK!  ← BL1正常跳转
EEEXXCX C ← 系统触发异常
PPPPPCCCCC:::00000xxxxx9f91790... ← 异常处理程序输出寄存器值
EXCPECC:0PxCf:09x40000000... ← 异常信息
```

### 问题分析

**异常触发点**: eret后立即触发异常，进入异常处理循环。

**可能原因**:

1. **SCTLR_EL2配置问题**
   - bootwrapper的SCTLR_EL2_RESET值可能不适合我们的环境
   - 当前值: `3 << 28 | 3 << 22 | 1 << 18 | 1 << 16 | 1 << 11 | 3 << 4`

2. **eret后第一条指令问题**
   - eret跳转到.hp232x_start_no_el3
   - 第一条指令`mov x0, xzr`可能触发异常

3. **寄存器状态问题**
   - eret前的寄存器状态可能不正确
   - SPSR_EL3配置可能有误

4. **硬件差异**
   - 我们的HP232X可能需要额外的初始化
   - bootwrapper可能针对特定硬件做了适配

### 异常输出分析

十六进制输出格式:
```
PPPPCCCCC:::00000xxxxx9f91790f470f0ef0100f633...
EXCPECC:0PxCf:09x40000000...
```

这看起来是某个异常处理程序的寄存器dump，可能来自RT-Thread的标准异常处理。

## 📋 下一步建议

### 建议1: 对比bootwrapper实际运行情况

**检查bootwrapper是否正常运行**:
```bash
# 测试bootwrapper本身是否能在HP232X正常启动
# 如果bootwrapper也触发异常，说明是硬件问题
# 如果bootwrapper正常，说明我们的实现有差异
```

### 建议2: 简化到最小启动

**最小化测试**:
1. 在eret后只做一件事：无限循环或简单UART输出
2. 看是否能稳定到达这个点
3. 然后逐步添加功能

### 建议3: 检查SCTLR_EL2_RESET值

**验证配置值**:
- bootwrapper的SCTLR_EL2_RESET定义: `(3 << 28 | 3 << 22 | 1 << 18 | 1 << 16 | 1 << 11 | 3 << 4)`
- 这个值的含义需要详细分析
- 可能需要针对HP232X做调整

### 建议4: 添加更精确的调试

**在关键点添加标记**:
- 在eret前输出标记（最后一次UART输出）
- 在eret后第一条指令前设置一个特殊标记
- 通过内存或硬件寄存器标记（不依赖UART）

### 建议5: 检查BL1环境

**验证BL1提供的环境**:
- BL1跳转时的寄存器状态
- BL1是否设置了必要的系统寄存器
- BL1是否启用了某些功能（如MMU、cache）

## 关键文件

### 修改的文件
- [entry_point.S](libcpu/aarch64/cortex-a/entry_point.S) - EL降级实现
- [link.lds](link.lds) - 内存布局
- [board.h](drivers/board.h) - 内存约束定义
- [rtconfig.h](rtconfig.h) - 配置开关

### 参考文件
- `/work/lynxi-bootwrapper/boot.S` - EL3初始化
- `/work/lynxi-bootwrapper/spin.S` - EL3→EL2降级
- `/work/lynxi-bootwrapper/common.S` - 定义和宏
- `/work/lynxi-bootwrapper/stack.S` - 栈设置

## 镜像信息

```
text:  194980字节 (~192KB)
data:  20144字节 (~20KB)
bss:   13056字节 (~13KB)
总计:  228180字节 (~224KB)
```

✅ 满足200-300KB目标
✅ 满足内存约束（IRAM0前256KB + IRAM1后256KB）

## 技术要点总结

### Bootwrapper EL降级关键步骤

1. **检查CurrentEL** - 确定是否需要EL3初始化
2. **检查MPIDR** - 确定主核还是次核
3. **设置SCR_EL3** - RES1位 + NS=1 + HVC=1 + RW=1
4. **设置SCTLR_EL2_RESET** - 预定义的复位值
5. **设置SPSR_EL3** - EL2h模式 + 异常掩码
6. **执行eret** - 降级到EL2
7. **清零寄存器** - kernel parameters
8. **再次检查MPIDR** - 确定主核/次核
9. **跳转到kernel** - 主核继续启动

### 内存约束验证

- **IRAM0**: 0x04000000-0x0403FFFF（前256KB）
- **IRAM1**: 0x100040000-0x10007FFFF（后256KB）
- **所有段都在约束范围内** ✅

## 结论

我们已经严格按照bootwrapper实现了完整的EL降级流程，满足所有内存约束。但是系统在eret后立即触发异常，需要进一步调试。

下一步应该对比bootwrapper的实际运行情况，或者进一步简化测试，找出根本原因。