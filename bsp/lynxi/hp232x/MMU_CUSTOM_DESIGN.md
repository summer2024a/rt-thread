# HP232X 自定义MMU映射方案

## 核心决策

**完全绕过RT-Thread MMU框架，实现极简静态映射**

### 为什么需要自定义方案？

RT-Thread MMU框架不适合HP232X：
1. `rt_page_init()` 需要**4MB连续内存** → IRAM1只有256KB可用 ❌
2. `rt_hw_mmu_setup()` 基于连续地址空间假设 → HP232X跨4GB边界 ❌
3. 页系统shadow_mask算法 → 需要4MB对齐 ❌
4. 动态页表分配 → 在IRAM1高地址，MMU启用前无法访问 ❌

### HP232X特殊内存布局

```
IRAM0: 0x04000000 (512KB) - 代码段，在4GB以下
IRAM1: 0x100000000 (512KB) - 数据段，在4GB以上！
UART:  0x10006000 - 外设，需要Device属性
GIC:   0x08000000 - 中断控制器，需要Device属性
```

## 设计方案

### 方案概述：Assembly阶段硬编码静态页表

**核心思路**：
- 在entry_point.S早期阶段完成MMU设置
- 使用硬编码的页表数据（存储在IRAM0的.text段）
- 只映射必需的最小区域
- 完全静态，无需动态计算或分配

**优势**：
- 最小代码路径，减少异常风险
- 避免C代码的内存访问问题
- 页表数据在IRAM0，MMU启用前可访问
- 极简实现，易于调试

### 最小化映射范围

**只映射真正需要的区域**（总共约2-3KB页表数据）：

```
必需映射：
├─ IRAM0 .text段
│  0x04000000-0x04080000 (512KB)
│  属性：Normal Memory (WB cacheable)
│  PMD indices: 32-35 (32个2MB块)
│
├─ UART
│  0x10006000-0x10007000 (4KB)
│  属性：Device Memory (nGnRnE)
│  PMD index: 128 (256MB区域的一个2MB块)
│
├─ GIC Distributor
│  0x08000000-0x08100000 (1MB)
│  属性：Device Memory
│  PMD indices: 64-71 (8个2MB块覆盖128MB区域)
│
└─ IRAM1 (数据段)
   0x100040000-0x100080000 (256KB，实际使用区域)
   属性：Normal Memory (WB cacheable)
   映射方式：PUD[4]使用1GB block descriptor
```

### 页表结构设计

#### Level 0 (PGD) - 只需要1个entry

```assembly
pgd[0]: 指向pud_table (覆盖VA 0-512GB)
```

#### Level 1 (PUD) - 只需要2个entries

```assembly
pud[0]: Table descriptor → pmd_table (0GB-1GB，混合属性)
pud[4]: Block descriptor → IRAM1 (4GB-5GB，Normal Memory)

Descriptor格式：
  pud[0] = (pmd_table_addr & ~0x3FF) | 0x3  // Table type
  pud[4] = (4 << 30) | 0x600 | 0x1          // Block, Normal Memory
```

#### Level 2 (PMD) - 只需要约150个entries

```assembly
PMD indices使用：
  [32-35]: IRAM0 (64MB，但实际只用512KB)
  [64-71]: GIC (128MB区域)
  [128]: UART (256MB区域)

Descriptor格式：
  pmd[i] = (i << 21) | attrs | 0x1

  IRAM0: attrs = 0x600 (Normal Memory)
  GIC/UART: attrs = 0x602 (Device Memory, AttrIndex=2)
```

### Assembly实现模板

```assembly
/* 在entry_point.S中添加 */

.section .text
.align 12
hp232x_static_pgd:
    .quad 0x0403b003              // pgd[0] → pud @ IRAM0, type=3 (table)
    
.align 12  
hp232x_static_pud:
    .quad 0x0403c003              // pud[0] → pmd @ IRAM0
    .quad 0                        // pud[1-3] unused
    .quad 0
    .quad 0
    .quad 0x100000601              // pud[4] → IRAM1 block @ 4GB
    
.align 12
hp232x_static_pmd:
    /* PMD[0-31]: unused */
    .rept 32
    .quad 0
    .endr
    
    /* PMD[32-35]: IRAM0 (64MB, Normal Memory) */
    .quad 0x04000601              // pmd[32] = (32<<21)|0x600|0x1
    .quad 0x04200601              // pmd[33]
    .quad 0x04400601              // pmd[34]
    .quad 0x04600601              // pmd[35]
    
    /* PMD[36-63]: unused */
    .rept 28
    .quad 0
    .endr
    
    /* PMD[64-71]: GIC (128MB area, Device Memory) */
    .quad 0x08000602              // pmd[64] = (64<<21)|0x602|0x1
    .quad 0x08200602              // pmd[65]
    // ... 重复到pmd[71]
    
    /* PMD[72-127]: unused */
    .rept 56
    .quad 0
    .endr
    
    /* PMD[128]: UART (256MB area, Device Memory) */
    .quad 0x10000602              // pmd[128] = (128<<21)|0x602|0x1

hp232x_enable_mmu_static:
    /* 1. 设置TTBR0_EL1 */
    ldr     x0, =hp232x_static_pgd
    msr     ttbr0_el1, x0
    isb
    
    /* 2. 设置TCR_EL1 */
    mov     x0, #(2 << 32)        // IPS=2 (40-bit PA, supports IRAM1)
    orr     x0, x0, #(3 << 12)    // SH0=3 (Inner Shareable)
    msr     tcr_el1, x0
    isb
    
    /* 3. 设置MAIR_EL1 */
    ldr     x0, =0x00447f         // Attr0=Normal WB, Attr2=Device
    msr     mair_el1, x0
    isb
    
    /* 4. 启用MMU */
    mrs     x0, sctlr_el1
    orr     x0, x0, #0x1          // M=1 (MMU enable)
    msr     sctlr_el1, x0
    isb
    
    ret
```

## 实现步骤

### Phase 1: 创建静态页表数据

**修改entry_point.S**：
1. 添加`hp232x_static_pgd/pud/pmd`数据段
2. 硬编码所有必需的descriptor值
3. 确保页表数据存储在IRAM0（地址<1GB）

### Phase 2: 实现MMU启用函数

**在entry_point.S添加**：
```assembly
hp232x_enable_mmu_static:
    // 设置TTBR0/TCR/MAIR
    // 启用MMU
    // 返回到调用者
```

### Phase 3: 调整调用时机

**修改entry_point.S启动流程**：
```
_start:
  ├─ MPIDR检测（多核隔离）
  ├─ EL降级（EL3→EL2→EL1）
  ├─ hp232x_enable_mmu_static（MMU启用）  ← 新增
  ├─ BSS清空（现在可以访问IRAM1）
  ├─ rtthread_startup
```

### Phase 4: 调整board.c

**简化board.c，移除手动MMU设置**：
```c
// 删除所有MMU配置代码
// 只保留：
//   - heap初始化（现在IRAM1可访问）
//   - GIC初始化
//   - UART初始化
//   - Timer初始化
```

## 关键优势

### 1. 极简实现
- 硬编码数据，无动态计算
- 最小代码路径（约100行assembly）
- 无需内存分配或复杂的页系统

### 2. 可靠性高
- 页表在IRAM0，MMU启用前可访问
- 完全静态，无运行时错误风险
- Assembly实现，避免C代码异常

### 3. 易于调试
- 每个descriptor值可预先验证
- 可用hexdump工具验证页表数据
- 问题定位简单（只有3个页表层级）

### 4. 性能可控
- 只映射必需区域，最小化TLB压力
- 精确控制cache属性
- 无复杂的页系统开销

## 验证方法

### 编译时验证

```bash
# 查看页表地址（确认在IRAM0）
aarch64-none-elf-nm rtthread.elf | grep hp232x_static

# 查看页表section位置
aarch64-none-elf-objdump -h rtthread.elf | grep hp232x_static

# 验证descriptor值
aarch64-none-elf-objdump -s -j .text rtthread.elf | grep -A20 "pgd"
```

### 运行时验证

```assembly
// 在MMU启用后添加调试输出
hp232x_enable_mmu_static:
    // ... MMU启用代码 ...
    
    // 验证MMU已启用
    mrs     x0, sctlr_el1
    and     x0, x0, #0x1
    cmp     x0, #1
    b.ne    mmu_failed
    
    // 测试IRAM1访问
    ldr     x0, =0x100040000
    ldr     x1, [x0]            // 如果成功，MMU工作正常
    
    // UART输出确认
    ldr     x0, =0x10006000
    mov     w1, #'M'            // "MMU OK"
    strb    w1, [x0]
    
    ret
```

## 预期结果

### 成功标志

1. ✅ MMU启用后UART正常输出
2. ✅ IRAM1 heap可正常访问
3. ✅ GIC中断控制器可初始化
4. ✅ 系统启动到RT-Thread shell

### 性能影响

- IRAM0代码执行：使用cache，性能良好
- IRAM1数据访问：使用cache，性能良好
- UART/GIC访问：Device属性，无cache，符合规范

## 后续优化

如果基本映射工作正常，可考虑扩展：

1. **添加更多映射区域**（按需）
   - 更多外设地址范围
   - 特殊功能区域

2. **调整cache属性**（优化）
   - 根据实际使用调整cache策略
   - 性能测试和调优

3. **多核支持**（扩展）
   - 每个CPU核的页表设置
   - SMP同步机制

## 文件修改清单

```
修改文件：
  entry_point.S:  添加静态页表数据 + hp232x_enable_mmu_static函数
  board.c:        移除MMU配置代码，简化初始化流程
  link.lds:       确认.text段足够容纳页表数据

删除代码：
  board.c中的所有手动MMU设置代码
  rt_page_init调用
  rt_hw_mmu_setup调用
  platform_mem_desc定义
```

## 总结

这个方案完全适配HP232X的特殊内存布局，通过硬编码静态页表实现最小化、可靠的MMU映射。相比RT-Thread框架：

- ✅ 无需4MB连续内存约束
- ✅ 支持跨4GB边界映射
- ✅ 页表数据在可访问位置
- ✅ 实现简单，易于调试
- ✅ 性能可控，无额外开销