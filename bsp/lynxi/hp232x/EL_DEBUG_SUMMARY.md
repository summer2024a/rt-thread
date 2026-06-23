# HP232X EL降级调试总结 (2026-06-23)

## 任务完成情况

### ✅ 已完成的工作

1. **内存规划制定** (MEMORY_PLAN.md)
   - 明确约束：IRAM0前256KB可用，IRAM1后256KB可用
   - 详细布局设计：代码段、数据段、页表、栈、堆的位置

2. **link.lds修改**
   - 调整.bss起始地址为0x100050000（IRAM1后256KB中间位置）
   - 添加详细的内存约束注释
   - 确保所有段在约束范围内

3. **board.h修改**
   - 添加详细的内存约束定义
   - 定义IRAM0_USE_START/END（前256KB）
   - 定义IRAM1_USE_START/END（后256KB）
   - 定义IRAM1_BSS_START（.bss起始地址）

4. **entry_point.S修改**（参考bootwrapper）
   - 完整的EL降级流程实现：
     - EL3初始化（参考boot.S）
       - SCR_EL3：RES1位 + NS=1 + HVC=1 + 64-bit EL2
       - CPTR_EL3：禁用协处理器陷阱
       - cntfrq_el0：定时器频率（31250000Hz）
       - CPUECTLR.SMPEn：多核同步（如果CPU支持）
     - EL2设置（参考spin.S）
       - SCTLR_EL2_RESET：预定义的复位值
       - CNTHCTL_EL2：定时器配置
       - ICC_SRE_EL2：GICv3系统寄存器访问
       - HCR_EL2：AArch64 EL1支持
   - 详细的调试输出标记

5. **编译验证**
   - 成功编译：229KB镜像（在200-300KB目标范围内）
   - 段布局验证：
     - IRAM0段：0x04000020-0x04035000（~208KB，在256KB范围内）
     - IRAM1段：.bss@0x100050000（在后256KB范围内）
   - PCIe Boot固件生成成功（212KB）

### ⚠️ 当前问题

**启动异常**：
- 输出："ECO POK! EEEXCC X XPPPCC:..."
- 系统在EL降级或早期阶段触发异常
- 进入异常处理循环

**可能原因**：
1. UART输出过多导致FIFO溢出或字符混乱
2. 异常向量未正确设置或异常处理逻辑有问题
3. 看门狗或其他硬件资源访问异常
4. 页表或内存访问问题

### 📋 下一步计划

**优先级1：简化调试输出**
- 减少UART输出字符数量
- 只保留关键阶段标记（EL级别、关键初始化点）
- 避免FIFO溢出导致输出混乱

**优先级2：验证EL降级**
- 使用更简单的方式验证EL降级是否成功
- 确认每个阶段的CurrentEL值
- 验证寄存器配置是否正确

**优先级3：排查异常原因**
- 分析异常向量表
- 确认异常处理逻辑
- 定位具体触发点

**优先级4：逐步测试**
- 先测试最小功能（EL降级 + UART输出）
- 再添加完整功能（GIC、Timer、MMU）

## 关键文件修改记录

| 文件 | 修改内容 | 影响 |
|------|---------|------|
| link.lds | .bss起始地址改为0x100050000 | 内存布局满足约束 |
| board.h | 添加详细内存约束定义 | 明确可用区域 |
| board.c | 修正宏引用IRAM1_USE_START | 编译错误修复 |
| entry_point.S | bootwrapper风格EL降级 | 完整EL初始化流程 |

## 技术要点总结

### Bootwrapper EL降级关键步骤

**EL3初始化** (boot.S:34-113)：
```assembly
1. 检查CurrentEL，如果在EL3：
   - SCR_EL3: 0x30 (RES1) + NS=1 + HVC=1 + RW=1
   - CPTR_EL3: 0 (禁用协处理器陷阱)
   - cntfrq_el0: 31250000 (定时器频率)
   - CPUECTLR.SMPEn: 0x40 (多核同步)
   - interconnect_init + gic_secure_init
2. Drop to EL2
```

**EL2设置** (spin.S:18-26)：
```assembly
1. SCTLR_EL2_RESET: 0x30C30C10
2. SPSR_KERNEL: 异常掩码 + EL2h模式
3. Drop to EL1/EL0
```

### 内存约束验证

**IRAM0使用情况**：
- 范围：0x04000000-0x0403FFFF（前256KB）
- 实际使用：0x04000020-0x04035000（~208KB）
- ✅ 满足约束（留有~48KB余量）

**IRAM1使用情况**：
- 范围：0x100040000-0x10007FFFF（后256KB）
- 实际使用：.bss@0x100050000（在后256KB内）
- ✅ 满足约束（前256KB完全保留）

### 镜像大小

- text: 195996字节 (~192KB)
- data: 16048字节 (~16KB)
- bss: 17152字节 (~17KB)
- 总计: 229196字节 (~224KB)
- ✅ 在200-300KB目标范围内

## 参考资料

1. `/work/lynxi-bootwrapper/boot.S` - EL3初始化流程
2. `/work/lynxi-bootwrapper/spin.S` - EL3→EL2降级
3. `/work/lynxi-bootwrapper/common.S` - SCTLR_EL2_RESET等定义
4. `/work/lynxi-bootwrapper/stack.S` - 栈设置
5. `/work/rt-thread/bsp/lynxi/hp232x/MEMORY_PLAN.md` - 内存规划文档
6. `/work/rt-thread/bsp/lynxi/hp232x/HANDOFF_SLIM.md` - 调试历史记录
7. `/work/rt-thread/bsp/lynxi/hp232x/TEST_METHODOLOGY.md` - 测试方法论

## 总结

本次调试完成了：
- ✅ 内存规划制定并实施
- ✅ Bootwrapper风格EL降级实现
- ✅ 编译成功并满足所有约束
- ⚠️ 启动流程仍有异常待排查

下一步重点是简化调试输出，逐步验证每个阶段的正确性，最终实现稳定启动。