# HP232X 内存规划 (Memory Layout Plan)

## 约束条件

根据硬件限制和调试需求：
- **IRAM0 (512KB @ 0x04000000)**: 只能使用**前256KB** (0x04000000-0x0403FFFF)
- **IRAM1 (512KB @ 0x100000000)**: 只能使用**后256KB** (0x100040000-0x10007FFFF)
- **镜像大小**: 目标 200-300KB

## 内存布局设计

### IRAM0 使用规划 (前256KB可用: 0x04000000-0x0403FFFF)

```
0x04000000 - 0x0400001F: PCIe Boot Header (32字节)
  - BL1 header metadata
  - 跳转地址: 0x04000020

0x04000020 - 0x040000FF: Entry Point Code (~224字节)
  - .text.entrypoint section
  - EL降级初始化代码
  - 主核检测代码

0x04000100 - 0x0403FFFF: Kernel Code + Data (~256KB - 224字节)
  - .text section (~192KB)
  - .rodata section
  - .data section (~16KB)
  - .mmu_table section (页表，必须放在IRAM0)
    - PGD: 4KB (512 entries)
    - PUD: 4KB (512 entries)
    - PMD: 4KB (512 entries for IRAM0/GIC混合映射)
  - 剩余空间预留

总计: ~256KB (约束上限)
```

### IRAM1 使用规划 (后256KB可用: 0x100040000-0x10007FFFF)

```
0x100040000 - 0x10004FFFF: CPU Stacks + Early Data (~64KB)
  - CPU stacks (8核 × 4KB = 32KB)
  - Early page pool (16KB)
  - Spin table mailbox (~1KB)
  - 预留空间

0x100050000 - 0x10005FFFF: .bss Section (~64KB)
  - 全局变量和静态变量
  - 当前大小: ~17KB
  - 预留空间供扩展

0x100060000 - 0x10006FFFF: Heap Area (~64KB)
  - RT-Thread内核堆
  - 动态内存分配

0x100070000 - 0x10007FFFF: Reserved (~64KB)
  - 预留给驱动和应用程序

总计: ~256KB (约束上限)
```

## 详细段布局

### IRAM0段分配

| 段名 | 起始地址 | 大小 | 用途 | 属性 |
|------|---------|------|------|------|
| .head | 0x04000000 | 32B | PCIe Boot Header | 只读 |
| .text.entrypoint | 0x04000020 | ~224B | 启动入口代码 | 只读执行 |
| .text | 0x04000100 | ~192KB | 内核代码 | 只读执行 |
| .rodata | (在.text后) | ~20KB | 常量数据 | 只读 |
| .data | (在.rodata后) | ~16KB | 初始化数据 | 可读写 |
| .mmu_table | (在.data后) | ~12KB | MMU页表 | 可读写 |
| **总计** | | **~256KB** | | |

### IRAM1段分配

| 段名 | 起始地址 | 大小 | 用途 | 属性 |
|------|---------|------|------|------|
| .cpu_stack | 0x100040000 | ~32KB | CPU栈(8核×4KB) | 可读写 |
| .early_pool | 0x100048000 | ~16KB | 早期页池 | 可读写 |
| .bss | 0x100050000 | ~64KB | 未初始化数据 | 可读写 |
| .heap | 0x100060000 | ~48KB | 内核堆 | 可读写 |
| .reserved | 0x100070000 | ~64KB | 预留空间 | 可读写 |
| **总计** | | **~256KB** | | |

## EL降级参考bootwrapper设计

### bootwrapper的EL降级流程

bootwrapper的实现简洁高效：

1. **检测CurrentEL**
   - 如果是EL3，执行EL3初始化
   - 否则跳过EL3初始化

2. **EL3初始化** (参考 boot.S: 34-113行)
   ```assembly
   // 1. 设置SCR_EL3
   mov x0, #0x30               // RES1 bits
   orr x0, x0, #(1 << 0)       // NS=1 (Non-secure EL1)
   orr x0, x0, #(1 << 8)       // HVC enable
   orr x0, x0, #(1 << 10)      // 64-bit EL2
   msr scr_el3, x0

   // 2. 禁用协处理器陷阱
   msr cptr_el3, xzr

   // 3. 设置定时器频率
   ldr x0, =cnt_freq
   ldr x0, [x0]
   msr cntfrq_el0, x0

   // 4. 设置CPUECTLR.SMPEn (多核同步)
   mrs x0, S3_1_c15_c2_1
   orr x0, x0, #0x40
   msr S3_1_c15_c2_1, x0

   // 5. 初始化interconnect和GIC
   bl interconnect_init
   bl gic_secure_init

   // 6. Drop to EL2
   b start_el3
   ```

3. **EL2设置** (参考 common.S: SCTLR_EL2_RESET)
   - 使用预定义的SCTLR_EL2_RESET值
   - 设置HCR_EL2支持AArch64 EL1
   - 设置SPSR_KERNEL (异常掩码)

### 我们需要实现的关键点

1. **简化EL降级流程**
   - 保持简洁，避免复杂的C代码初始化
   - 在assembly阶段完成大部分工作

2. **关键寄存器配置**
   - SCR_EL3: RES1位 + NS=1 + HVC=1 + 64-bit EL2
   - SCTLR_EL2: 使用bootwrapper的SCTLR_EL2_RESET定义
   - HCR_EL2: RW=1 (AArch64 EL1)
   - cntfrq_el0: 31250000 Hz

3. **避免过早的MMU启用**
   - bootwrapper不启用MMU
   - 让C代码在board.c中按需配置MMU
   - 页表放在IRAM0的.mmu_table section

## 内存访问验证

### CPU在无MMU时可以访问IRAM1 ✅

ARMv8-A关键特性：
- 支持40-bit物理地址（0x0 - 0xFFFFFFFFFF）
- IRAM1 @ 0x100000000 (4GB) 在40-bit范围内
- **即使MMU未启用，CPU仍可直接访问IRAM1**

验证：
- CurrentEL: 任何级别（EL1/EL2/EL3）
- MMU状态: 未启用（SCTLR_EL1.M=0）
- 结果: CPU可以访问IRAM1数据（栈、bss、heap）

### 页表必须放在IRAM0 ⚠️

虽然MMU未启用时可以访问IRAM1，但**页表本身必须放在IRAM0**：
- 原因：TTBR0_EL1寄存器只能指向<4GB的地址
- IRAM1地址(0x100000000)超出TTBR0_EL1的范围限制
- 解决方案：link.lds中定义.mmu_table section在IRAM0

## 修改计划

### 1. link.lds修改

当前link.lds需要调整：
- .bss起始地址改为0x100050000（IRAM1后256KB的中间位置）
- .mmu_table section必须保留在IRAM0
- 确保所有段都在约束范围内

### 2. board.h修改

定义新的内存边界：
```c
#define IRAM0_START         0x04000000UL
#define IRAM0_END           0x04040000UL     // 前256KB上限
#define IRAM1_HIGH_START    0x100040000UL   // 后256KB起始
#define IRAM1_END           0x100080000UL   // 512KB结束
```

### 3. entry_point.S修改

参考bootwrapper的EL降级：
- 添加完整的SCR_EL3配置（RES1位）
- 使用SCTLR_EL2_RESET定义
- 简化降级流程，避免复杂的C代码初始化

## 测试验证

### 编译验证
```bash
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build
scons -j$(nproc)
aarch64-none-elf-size rtthread.elf  # 确认text+data+bss < 256KB
aarch64-none-elf-objdump -h rtthread.elf  # 确认段地址在约束范围
```

### 启动验证
```bash
python3 mkimage.py rtthread.bin rtthread-header.bin
python3 remote_test.py
# 预期输出：
# [I/board] STEP1: Skip rt_page_init ✅
# [I/board] STEP2: Manual MMU setup ✅
# [I/board] STEP3: Enable MMU ✅
# Banner显示 ✅
```

## 总结

本规划确保：
- ✅ IRAM0使用控制在前256KB
- ✅ IRAM1使用控制在后256KB
- ✅ 页表正确放置在IRAM0
- ✅ EL降级参考bootwrapper实现
- ✅ 避免过早的MMU启用
- ✅ 镜像大小控制在200-300KB

下一步：修改link.lds和entry_point.S实现此规划。