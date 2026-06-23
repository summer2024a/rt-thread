# HP232X MMU页表设计方案

## 🎉 重大突破 (2026-06-23 16:40)

### ✅ MMU完全启用成功！

**测试结果**：
```
ECO
POK!
IBKMPDUATB1RCGgUuTt

 \ | /
- RT -     Thread Operating System
 / | \     5.3.0 build Jun 23 2026 08:40:09
 2006 - 2024 Copyright by RT-Thread team
```

**调试标记解析** (`IBKMPDUATB1RCGgUuTt`)：
- **I/B/K** = LOG_I系统输出开始
- **M** = MMU页表初始化开始
- **P** = PMD页表配置
- **D** = PMD配置完成
- **U** = PUD配置完成
- **A** = MAIR_EL1配置前
- **T** = TCR_EL1配置后
- **B** = MMU启用前
- **1** = MMU已启用 ✅
- **R** = IRAM1访问测试成功 ✅
- **C** = Cache已启用 ✅
- **G** = GIC中断初始化前
- **g** = GIC中断初始化成功 ✅
- **U** = UART初始化前
- **u** = UART初始化成功 ✅
- **T** = Timer初始化前
- **t** = Timer初始化成功 ✅

**完整里程碑成就**：
1. ✅ **页表配置成功** - PGD/PUD/PMD都在IRAM0，12KB页表空间
2. ✅ **MMU启用成功** - SCTLR_EL1.M=1，分阶段启用（先MMU后Cache）
3. ✅ **IRAM1访问成功** - 通过MMU访问4GB边界外的IRAM1
4. ✅ **Cache启用成功** - 数据Cache和指令Cache都启用
5. ✅ **GIC中断初始化** - GICv3中断控制器工作
6. ✅ **UART驱动初始化** - 串口驱动正常工作
7. ✅ **Timer初始化** - Generic Timer工作
8. ✅ **Kernel启动成功** - RT-Thread Banner完整显示

**当前状态**：
- ⚠️ Shell未显示（应用初始化阶段需要进一步调试）
- ✅ 所有板级初始化（board.c）完成
- ✅ MMU页表机制完全工作

**页表地址验证**：
```
hp232x_pgd    @ 0x04036000 (IRAM0) ✅
pud_table_0   @ 0x04037000 (IRAM0) ✅
pmd_table_0   @ 0x04038000 (IRAM0) ✅
```

所有页表都在IRAM0前256KB范围内，符合设计要求！

---

## 当前问题分析

### 1. UART地址映射错误
**问题**：UART地址 `0x10006000` 不在IRAM0范围内
- IRAM0: 0x04000000 - 0x040FFFFF (1MB)
- UART: 0x10006000 = 256MB + 24KB

**根因**：UART在256MB区域，需要独立的PMD映射

### 2. 页表存储位置问题
**问题**：当前页表定义被`#if 0`禁用，节省空间
**根因**：担心IRAM1地址>4GB，MMU启用前无法访问

**事实**：ARMv8-A CPU在无MMU时仍可访问40-bit物理地址（包括IRAM1 @ 4GB）
**结论**：页表可以放在IRAM0或IRAM1，但推荐放在IRAM0（更安全）

### 3. Descriptor格式错误
**问题**：之前SH field位置错误
**正确格式**：
```
Block Descriptor (2MB):
  Bits [47:21]: Output Address (phys_addr >> 21)
  Bits [11:10]: SH (Shareability)
  Bit [10]: AF (Access Flag)
  Bits [9:8]: AP (Access Permissions)
  Bits [4:2]: AttrIndex (MAIR index)
  Bits [1:0]: Type = 1 (block)
```

**解决方案**：使用RT-Thread标准宏（已包含正确配置）

## MMU页表设计

### 地址映射策略

#### Identity Mapping (VA = PA)

```
Virtual Address Space:
  0x00000000 - 0x04000000: Unused (0-64MB)
  0x04000000 - 0x08000000: IRAM0 (64MB, NORMAL_MEM)
  0x08000000 - 0x10000000: GIC (64MB, DEVICE_MEM)
  0x10000000 - 0x20000000: Peripherals (256MB, DEVICE_MEM)
    - 0x10006000: UART0
  0x100000000 - 0x100080000: IRAM1 (512MB, NORMAL_MEM)
```

### 页表结构（3级页表）

#### Level 0 (PGD)
```
PGD: 512 entries, each covers 512GB

pgd[0] → pud_table_0 (VA 0-512GB)
pgd[2] → pud_table_2 (VA 1024GB-1536GB, 包含IRAM1)
```

#### Level 1 (PUD)
```
PUD: 512 entries, each covers 1GB

pud_table_0[0] → pmd_table_0 (VA 0-1GB, 混合属性)
pud_table_0[1-3] → 1GB block DEVICE (VA 1-4GB)
pud_table_0[4] → 1GB block NORMAL (VA 4-5GB, IRAM1)  ← 关键映射
```

#### Level 2 (PMD)
```
PMD: 512 entries, each covers 2MB

pmd_table_0:
  [0-31]   → 2MB block DEVICE (VA 0-64MB)
  [32-63]  → 2MB block NORMAL (VA 64-128MB, IRAM0) ← IRAM0映射
  [64-127] → 2MB block DEVICE (VA 128-256MB, GIC) ← GIC映射
  [128-255] → 2MB block DEVICE (VA 256-512MB, UART等) ← UART映射
```

### 页表存储位置

**推荐**：所有页表放在IRAM0的`.mmu_table` section
**原因**：
1. IRAM0地址<4GB，MMU启用前后都可访问
2. 更安全，避免潜在问题
3. 空间充足（IRAM0前256KB可用）

**布局**：
```
IRAM0 (.mmu_table section):
  PGD:    4KB (512 entries)
  PUD_0:  4KB (512 entries)
  PMD_0:  4KB (512 entries)
  Total: 12KB页表空间
```

### MMU控制寄存器配置

#### MAIR_EL1 (Memory Attribute Indirection Register)
```
AttrIdx 0 (NORMAL_MEM): 0x7f (WB cacheable)
AttrIdx 1 (NORMAL_NC):  0x44 (Non-cacheable)
AttrIdx 2 (DEVICE_MEM): 0x00 (Device nGnRnE)

MAIR_EL1 = 0x00447fUL
```

#### TCR_EL1 (Translation Control Register)
```
IPS = 2 (40-bit PA, supports IRAM1 @ 4GB)
TG0 = 0 (4KB granule)
SH0 = 3 (Inner Shareable)
ORGN0 = 1 (Outer Cacheable)
IRGN0 = 1 (Inner Cacheable)
T0SZ = 0 (64-bit VA space)

TCR_EL1 = 0x0000003500000000UL
```

#### TTBR0_EL1
```
TTBR0_EL1 = pgd_base_address (IRAM0中)
```

#### SCTLR_EL1
```
Phase 1: Enable MMU only (M=1, C=0, I=0)
Phase 2: Enable caches (M=1, C=1, I=1)
```

## 实现步骤

### Step 1: 启用页表定义
- 移除`#if 0`禁用
- 定义页表在`.mmu_table` section
- 确保4KB对齐

### Step 2: 页表初始化
- 清空所有页表
- 设置PGD→PUD链接
- 设置PUD→PMD链接（VA 0-1GB）
- 设置PMD entries（IRAM0, GIC, UART）
- 设置PUD block（IRAM1）

### Step 3: MMU启用流程
1. 配置MAIR_EL1
2. 配置TCR_EL1
3. 设置TTBR0_EL1
4. Data Synchronization Barrier (dsb sy)
5. Enable MMU (SCTLR_EL1.M=1)
6. Instruction Synchronization Barrier (isb)
7. 验证IRAM1访问
8. Enable caches (SCTLR_EL1.C=1, I=1)

### Step 4: 调试输出
- 使用`early_putc_direct()`标记关键步骤
- 使用LOG_I输出配置值
- 验证descriptor格式正确性

## 关键计算

### PMD Index计算
```
地址 → PMD index:
  IRAM0 @ 0x04000000: idx = 0x04000000 / 2MB = 32
  GIC  @ 0x08000000:  idx = 0x08000000 / 2MB = 64
  UART @ 0x10006000:  idx = 0x10000000 / 2MB = 128
```

### Descriptor值计算
```
PMD Block Descriptor:
  descriptor = (pmd_index << 21) | MMU_MAP_K_RWCB | MMU_TYPE_BLOCK

IRAM0 (idx=32):
  descriptor = (32 << 21) | 0x600 | 0x1 = 0x04000601

GIC (idx=64):
  descriptor = (64 << 21) | 0x600 | 0x1 = 0x08000601

UART (idx=128):
  descriptor = (128 << 21) | 0x600 | 0x1 = 0x10000601

IRAM1 (PUD block, idx=4):
  descriptor = (4 << 30) | 0x600 | 0x1 = 0x100000601
```

## 验证方法

### 1. 页表地址验证
```bash
aarch64-none-elf-nm rtthread.elf | grep hp232x_pgd
# 应输出: 0x040xxxxx (IRAM0地址)
```

### 2. Descriptor格式验证
```c
LOG_I("pmd[32]=0x%lx (expect 0x04000601)", pmd_table_0[32]);
LOG_I("pud[4]=0x%llx (expect 0x100000601)", pud_table_0[4]);
```

### 3. MMU启用验证
```c
// 读取SCTLR_EL1
unsigned long sctlr;
asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
LOG_I("SCTLR_EL1: M=%d C=%d I=%d",
      (sctlr & 1), (sctlr & 4), (sctlr & 0x1000));

// 测试IRAM1访问
volatile unsigned long *iram1 = (volatile unsigned long *)0x100040000;
unsigned long val = *iram1;
LOG_I("IRAM1 read: 0x%lx", val);
```

## 注意事项

### 1. UART地址特殊处理
UART在256MB区域（PMD index 128），不是在IRAM0范围内
需要独立的PMD entry，使用DEVICE_MEM属性

### 2. IRAM1映射关键
IRAM1 @ 4GB边界，必须使用PUD level映射（1GB block）
PUD index = 4 (对应4GB-5GB范围)

### 3. 页表大小限制
总页表大小：12KB（PGD + PUD + PMD）
IRAM0前256KB空间充足

### 4. Memory Constraints
- IRAM0: 只使用前256KB（0x04000000-0x0403FFFF）
- IRAM1: 只使用后256KB（0x100040000-0x10007FFFF）
- 页表放在IRAM0，bss/heap放在IRAM1

## 下一步行动

1. ✅ 分析完成，设计方案已确定
2. ⏳ 修改board.c实现页表配置
3. ⏳ 修改linker script确认页表位置
4. ⏳ 编译测试验证MMU启用
5. ⏳ 调试并修复问题（如有）