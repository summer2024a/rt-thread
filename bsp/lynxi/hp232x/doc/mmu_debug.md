# HP232X MMU页表配置要点

## 内存约束

### HP232X硬件限制
- IRAM0: 前256KB保留 (0x04000000-0x0403FFFF) — bootwrapper + SPL
- IRAM0: 后256KB可用 (0x04040000-0x0407FFFF) — kernel text/data/mmu_table
- IRAM1: 前256KB保留 (0x100000000-0x10003FFFF) — 不可使用
- IRAM1: 后256KB可用 (0x100040000-0x10007FFFF) — BSS + heap + stack

### 页表位置要求
- 页表必须放在IRAM0（避免跨4GB边界）
- 需要12KB页表空间（PGD 4KB + PUD 4KB + PMD 4KB）
- 页表放在 `.mmu_table` section，链接在 IRAM0 后256KB 内

## 页表配置方案

### Identity Mapping策略
- IRAM0 @ 0x04040000：Normal Memory（WB Cache）— **后256KB**
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
  PMD[258-259] → IRAM0 (0x04040000-0x04080000, Normal)
  PMD[64-127] → GIC (128MB-256MB, Device)
  PMD[128-255] → UART/peripherals (256MB-512MB, Device)
```

**IRAM0 PMD索引计算**：
- PMD index = VA >> 21（每个PMD条目覆盖2MB）
- IRAM0 @ 0x04040000: PMD[258] = 0x04040000 >> 21
- IRAM0 end @ 0x04080000: PMD[259]

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

// IRAM0 @ 0x04040000 (2MB block, PMD[258])
pmd_table[258] = (258 << 21) | 0x600 | 0x1 = 0x04080601

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
  IPS=2: 40-bit Physical Address (支持IRAM1 @ 0x100000000)
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
// Phase 1: 设置MAIR_EL1
ldr     x0, =0x00447f
msr     mair_el1, x0
isb

// Phase 2: 设置TCR_EL1 (IPS=2 for 40-bit PA)
ldr     x0, =tcr_config
msr     tcr_el1, x0
isb

// Phase 3: 设置TTBR0
ldr     x0, =pgd_base
msr     ttbr0_el1, x0
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

// Phase 6: 切换IRAM1栈
movz    x0, #0xfffc, lsl #0       /* bits 0-15 */
movk    x0, #0x7, lsl #16         /* bits 16-31 */
movk    x0, #0x1, lsl #32         /* bits 32-47 */
mov     sp, x0                    /* sp = 0x10007FFFC */
isb
```

## 调试要点

### 1. 页表对齐
- PGD/PUD/PMD必须4KB对齐（.align 12）
- 使用section(".mmu_table")确保位置在IRAM0

### 2. Descriptor计算
```c
// 错误：直接叠加地址
pud_table[4] = 0x100000000 | MMU_MAP_K_RWCB | MMU_TYPE_BLOCK;

// 正确：使用索引左移
pud_table[4] = (4 << 30) | 0x600 | 0x1;
```

### 3. IRAM0地址变更
- IRAM0 kernel 从 0x04000000 移到 0x04040000
- PMD索引从 [32-63] 变为 [258-259]
- 页表仍放在IRAM0 `.mmu_table` section

### 4. TLB维护
```assembly
// 启用MMU后需要TLB失效
tlbi    vmalle1is
dsb     ish
isb
```

## 调试标记解读

**board.c输出**：`1ABCDEFGHIJKLMNOPQRSTXYABCDE...`

| 标记 | 含义 | 检查点 |
|------|------|--------|
| 1 | MMU配置开始 | 页表清零 |
| A/B/C/D | PMD配置 | IRAM0/GIC/UART映射 |
| E | PUD loop完成 | |
| F | IRAM1 PUD[4] | 4GB边界映射 |
| G/J/K | 验证描述符 | |
| L | MAIR_EL1 | 内存属性 |
| M | MAIR验证 | |
| N | TCR_EL1 | IPS=2 (40-bit PA) |
| O | TTBR0_EL1 | 页表基址 |
| P/Q | MMU Phase 1 | SCTLR_EL1.M=1 |
| R/S | MMU启用完成 | |
| T/X/Y | BSS清零 | IRAM1 40-bit PA |
| A/B/C | 栈切换 | sp=0x10007FFFC |
| D/E | Heap init | |
| F/G/H | GIC init | |

## 常见问题

### 1. MMU启用后系统挂起
**原因**：页表Descriptor格式错误或未对齐
**解决**：检查Descriptor计算和页表对齐

### 2. 无法访问IRAM0新地址
**原因**：PMD索引未更新（从32-63改为258-259）
**解决**：确保 PMD[258-259] 映射 IRAM0 @ 0x04040000

### 3. 无法访问IRAM1
**原因**：未正确映射4GB边界
**解决**：PUD[4]配置1GB块映射，TCR_EL1 IPS=2

### 4. Cache启用后数据异常
**原因**：Memory Attribute配置错误
**解决**：检查MAIR_EL1和Descriptor MA field

## 参考文档
- ARMv8-A Architecture Reference Manual
- HANDOFF_SLIM.md（项目进展）
- TEST_METHODOLOGY.md（测试方法）
