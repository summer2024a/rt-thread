# HP232X MMU页表配置要点

## 内存约束

### HP232X硬件限制
- IRAM0: 前256KB可用 (0x04000000-0x0403FFFF)
- IRAM0: 后256KB保留 (0x04040000-0x0407FFFF)
- IRAM1: 前256KB保留 (0x100000000-0x10003FFFF)
- IRAM1: 后256KB可用 (0x100040000-0x10007FFFF)

### 页表位置要求
- 页表必须放在IRAM0（避免跨4GB边界）
- 需要12KB页表空间（PGD 4KB + PUD 4KB + PMD 4KB）

## 页表配置方案

### Identity Mapping策略
- IRAM0 @ 0x04000000：Normal Memory（WB Cache）
- IRAM1 @ 0x100000000：Normal Memory（WB Cache）
- GIC @ 0x08000000：Device Memory（nGnRnE）
- UART @ 0x10006000：Device Memory

### 3级页表结构
```
PGD (Level 0, 512 entries):
  PGD[0] → PUD table (covers VA 0-512GB)

PUD (Level 1, 512 entries):
  PUD[0] → PMD table (covers VA 0GB-1GB)
  PUD[4] → 1GB block for IRAM1 (VA 4GB, PA 4GB)

PMD (Level 2, 512 entries):
  PMD[32-35] → IRAM0 (64MB-72MB, Normal)
  PMD[64-71] → GIC (128MB-144MB, Device)
  PMD[128] → UART (256MB, Device)
```

## Descriptor格式

### Block Descriptor
```
OA field (Output Address):
  Level 1 Block (1GB): bits [47:30] = GB_index << 30
  Level 2 Block (2MB): bits [47:21] = 2MB_index << 21

Attributes:
  AF=1, SH=OuterShareable, AP=RW@EL1

Type:
  Block: 0x1
  Table: 0x3
```

### 示例
```c
// IRAM1 @ 4GB (1GB block)
pud_table[4] = (4 << 30) | 0x600 | 0x1 = 0x100000601

// IRAM0 @ 64MB (2MB block)
pmd_table[32] = (32 << 21) | 0x600 | 0x1 = 0x04000601

// GIC @ 128MB (Device memory)
pmd_table[64] = (64 << 21) | 0x602 | 0x1 = 0x08000602
```

## MAIR_EL1配置

### Memory Attribute Indirection Register
```c
MAIR_EL1 = 0x00447fUL
  Attr0: Normal WB cacheable (MA field = 0)
  Attr1: Normal NC (MA field = 1)
  Attr2: Device nGnRnE (MA field = 2)
```

### 使用示例
```c
// Normal Memory WB
MMU_MAP_K_RWCB = AF=1, SH=Outer, AP=RW@EL1, MA=0

// Device Memory
MMU_MAP_K_DEVICE = AF=1, SH=Outer, AP=RW@EL1, MA=2
```

## TCR_EL1配置

### Translation Control Register
```c
TCR_EL1配置:
  IPS=0: 32-bit Physical Address
  TG0=4KB: Granule size
  SH0=InnerShareable: Shareability
  ORGN0/IRGN0=Normal WB: Cacheability
```

### 关键位域
```
IPS (bits [34:32]): Physical Address Size
TG0 (bits [15:14]): Granule Size
SH0 (bits [13:12]): Shareability
ORGN0 (bits [11:10]): Outer Cacheability
IRGN0 (bits [9:8]): Inner Cacheability
```

## MMU启用流程

### 分阶段启用（避免异常）
```assembly
// Phase 1: 设置TTBR0
ldr     x0, =pgd_base
msr     ttbr0_el1, x0
isb

// Phase 2: 设置MAIR_EL1
ldr     x0, =0x00447f
msr     mair_el1, x0
isb

// Phase 3: 设置TCR_EL1
ldr     x0, =tcr_config
msr     tcr_el1, x0
isb

// Phase 4: 启用MMU（不启用Cache）
mrs     x0, sctlr_el1
orr     x0, x0, #0x1              /* Set M bit */
msr     sctlr_el1, x0
isb

// Phase 5: 启用Cache
mrs     x0, sctlr_el1
orr     x0, x0, #0x4              /* Data Cache */
orr     x0, x0, #0x1000           /* Instruction Cache */
msr     sctlr_el1, x0
isb
```

## 调试要点

### 1. 页表对齐
- PGD/PUD/PMD必须4KB对齐（.align 12）
- 使用section(".mmu_table")确保位置

### 2. Descriptor计算
```c
// 错误：直接叠加地址
pud_table[4] = 0x100000000 | MMU_MAP_K_RWCB | MMU_TYPE_BLOCK;

// 正确：使用索引左移
pud_table[4] = (4 << 30) | 0x600 | 0x1;
```

### 3. IRAM1访问
- CPU可以在MMU未启用时访问IRAM1（40-bit物理地址）
- MMU启用后需要正确映射

### 4. TLB维护
```assembly
// 启用MMU后需要TLB失效
tlbi    vmalle1is
dsb     ish
isb
```

## 调试标记解读

**board.c输出**：`IBKMPDUATB1RCG`

| 标记 | 含义 | 检查点 |
|------|------|--------|
| I/B/K | LOG_I输出 | C代码执行 |
| M | MMU初始化开始 | 页表配置 |
| P/D | PMD配置完成 | 2MB块映射 |
| U/A | PUD配置完成 | 1GB块映射 |
| T | TCR_EL1配置 | Translation控制 |
| B | MMU启用前 | SCTLR_EL1.M=0 |
| 1 | MMU已启用 ✅ | SCTLR_EL1.M=1 |
| R | IRAM1访问测试 | 4GB边界验证 |
| C | Cache已启用 ✅ | SCTLR_EL1.C/I=1 |

## 常见问题

### 1. MMU启用后系统挂起
**原因**：页表Descriptor格式错误或未对齐
**解决**：检查Descriptor计算和页表对齐

### 2. 无法访问IRAM1
**原因**：未正确映射4GB边界
**解决**：PUD[4]配置1GB块映射

### 3. Cache启用后数据异常
**原因**：Memory Attribute配置错误
**解决**：检查MAIR_EL1和Descriptor MA field

## 参考文档
- ARMv8-A Architecture Reference Manual
- MMU_DESIGN.md（详细设计）
- MMU_SUCCESS_SUMMARY.md（成功记录）