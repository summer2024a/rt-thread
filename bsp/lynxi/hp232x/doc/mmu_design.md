# HP232X MMU Design

## 1. 设计目标

本文档描述 HP232X 平台在 `board` 阶段的 MMU 页表设计方案。

目标：

1. 使用 AArch64 EL1 stage-1 translation。
2. 使用 4KB granule。
3. 使用 identity mapping，即 VA == PA。
4. 精确映射片上 RAM：
   - RAM0: `0x04000000 ~ 0x0407ffff`
   - RAM1: `0x100000000 ~ 0x10007ffff`
5. 映射 `lynchip-lite-base.md` 中列出的 IP 外设地址。
6. 不映射 DDR memory 区域。
7. 页表放置在 IRAM0 可用区域内。
8. 支持 4GB 以上物理地址，包括 RAM1 和 APU。

---

## 2. 地址范围

### 2.1 RAM 区域

当前 RAM 映射范围如下：

| 区域 | 起始地址 | 结束地址 | 大小 | 属性 |
|---|---:|---:|---:|---|
| RAM0 / IRAM0 | `0x04000000` | `0x0407ffff` | 512KB | Normal WB |
| RAM1 / IRAM1 | `0x100000000` | `0x10007ffff` | 512KB | Normal WB |

说明：

- RAM0/RAM1 均只映射 512KB。
- 为了精确控制映射范围，RAM0/RAM1 不能使用 2MB block 映射。
- RAM0/RAM1 必须使用 L3 4KB page 映射。

---

### 2.2 软件使用约束

虽然 MMU 映射整个 512KB RAM0/RAM1，但软件可使用区域仍可按启动约束划分：

| 区域 | 地址范围 | 用途 |
|---|---|---|
| RAM0 前 256KB | `0x04000000 ~ 0x0403ffff` | bootwrapper / SPL / 保留 |
| RAM0 后 256KB | `0x04040000 ~ 0x0407ffff` | kernel text/data/mmu_table |
| RAM1 前 256KB | `0x100000000 ~ 0x10003ffff` | 保留 |
| RAM1 后 256KB | `0x100040000 ~ 0x10007ffff` | BSS / heap / stack |

注意：

> “MMU 映射范围”与“软件实际使用范围”不是一回事。  
> 当前设计映射完整 RAM0/RAM1 512KB，但 linker/runtime 可以只使用后 256KB。

---

### 2.3 外设/IP 区域

根据 `lynchip-lite-base.md`，需要映射如下 IP 地址区域。

| 模块 | PA | 2MB 对齐块基地址 | L1 index | L2 index |
|---|---:|---:|---:|---:|
| AHB boot ROM/IRAM | `0x00000000` | `0x00000000` | 0 | 0 |
| SPI1/SFC AHB | `0x02000000` | `0x02000000` | 0 | 16 |
| IRAM0 | `0x04000000` | `0x04000000` | 0 | 32 |
| IRAM0 Sys IRAM | `0x06000000` | `0x06000000` | 0 | 48 |
| GICD | `0x08000000` | `0x08000000` | 0 | 64 |
| GIC ITS | `0x08020000` | `0x08000000` | 0 | 64 |
| GICR | `0x08100000` | `0x08000000` | 0 | 64 |
| Core Timer Ctrl | `0x08600000` | `0x08600000` | 0 | 67 |
| Interconnect/CCM | `0x09000000` | `0x09000000` | 0 | 72 |
| I2C0 | `0x10002000` | `0x10000000` | 0 | 128 |
| I2C1 | `0x10003000` | `0x10000000` | 0 | 128 |
| I2C2 | `0x10004000` | `0x10000000` | 0 | 128 |
| I2C3 | `0x10005000` | `0x10000000` | 0 | 128 |
| UART0 | `0x10006000` | `0x10000000` | 0 | 128 |
| UART1 | `0x10007000` | `0x10000000` | 0 | 128 |
| SPI0/SFC | `0x1000a000` | `0x10000000` | 0 | 128 |
| SPI2 | `0x1000b000` | `0x10000000` | 0 | 128 |
| GPIO | `0x1000e000` | `0x10000000` | 0 | 128 |
| WDT | `0x10010000` | `0x10000000` | 0 | 128 |
| System Timer | `0x10012000` | `0x10000000` | 0 | 128 |
| RTC | `0x10014000` | `0x10000000` | 0 | 128 |
| DMA | `0x1001a000` | `0x10000000` | 0 | 128 |
| ETH | `0x10020000` | `0x10000000` | 0 | 128 |
| MMC/SD | `0x10040000` | `0x10000000` | 0 | 128 |
| PINCTRL | `0x12000000` | `0x12000000` | 0 | 144 |
| PVT | `0x12200000` | `0x12200000` | 0 | 145 |
| EFUSE | `0x12300000` | `0x12200000` | 0 | 145 |
| CPR/RESET | `0x12500000` | `0x12400000` | 0 | 146 |
| EDAC_MC | `0x13000000` | `0x13000000` | 0 | 152 |
| VPSS | `0x18000000` | `0x18000000` | 0 | 192 |
| ROTATION | `0x18020000` | `0x18000000` | 0 | 192 |
| MVE GPU | `0x19000000` | `0x19000000` | 0 | 200 |
| PCIe EP/RC DBI | `0x1a000000` | `0x1a000000` | 0 | 208 |
| PCIe SDBand | `0x1b000000` | `0x1b000000` | 0 | 216 |
| IRAM1 | `0x100000000` | `0x100000000` | 4 | 0 |
| APU | `0x1000000000` | `0x1000000000` | 64 | 0 |

---

## 3. 页表层级设计

### 3.1 TCR 配置假设

建议采用：

```text
Granule: 4KB
VA size: 39-bit
PA size: 40-bit

对应
TCR_EL1.T0SZ = 25
TCR_EL1.TG0  = 4KB
TCR_EL1.IPS  = 2，即 40-bit PA

39-bit VA 可覆盖512GB 地址空间：0x0000000000 ~ 0x7fffffffff
```

### 3.2 页表层级
在 4KB granule + 39-bit VA 下，TTBR0_EL1 指向 Level 1 table。

页表索引关系：

```
L1 index = VA[38:30] = VA >> 30
L2 index = VA[29:21] = (VA >> 21) & 0x1ff
L3 index = VA[20:12] = (VA >> 12) & 0x1ff
```
每级覆盖范围：
---
| Level |	每个 entry覆盖大小 |	用途 |
| L1 |	1GB	| 指向 L2 table 或 1GB block |
| L2	| 2MB	| 指向 L3 table 或 2MB block |
| L3	| 4KB	| 4KB page |
---

## 4. 总体页表结构

```
L1 table
 ├── L1[0]  -> L2_LOW table
 │             covers 0x00000000 ~ 0x3fffffff
 │
 │             L2[0]    -> Boot ROM / AHB IRAM, Device or Normal-NC
 │             L2[16]   -> SPI1/SFC AHB, Device
 │             L2[32]   -> L3_RAM0 table
 │             L2[48]   -> IRAM0 Sys IRAM, Normal or Device
 │             L2[64]   -> GICD/GICR/GIC ITS, Device
 │             L2[67]   -> Core Timer Ctrl, Device
 │             L2[72]   -> Interconnect/CCM, Device
 │             L2[128]  -> I2C/UART/SPI/GPIO/WDT/TIMER/DMA/ETH/MMC, Device
 │             L2[144]  -> PINCTRL, Device
 │             L2[145]  -> PVT/EFUSE, Device
 │             L2[146]  -> CPR/RESET, Device
 │             L2[152]  -> EDAC_MC, Device
 │             L2[192]  -> VPSS/ROTATION, Device
 │             L2[200]  -> MVE GPU, Device
 │             L2[208]  -> PCIe EP/RC DBI, Device
 │             L2[216]  -> PCIe SDBand, Device
 │
 ├── L1[4]  -> L2_RAM1 table
 │             covers 0x100000000 ~ 0x13fffffff
 │
 │             L2[0] -> L3_RAM1 table
 │                       L3[0..127] -> 0x100000000 ~ 0x10007ffff
 │
 └── L1[64] -> L2_APU table
               covers 0x1000000000 ~ 0x103fffffff

               L2[0] -> APU Device block
```

## 5. RAM0/RAM1 精确映射设计

### 5.1 RAM0

RAM0 地址范围：

```text
0x04000000 ~ 0x0407ffff
```

大小：

```text
512KB = 128 * 4KB
```

索引计算：

```text
L1 index = 0x04000000 >> 30 = 0
L2 index = (0x04000000 >> 21) & 0x1ff = 32
L3 index = (0x04000000 >> 12) & 0x1ff = 0
```

页表结构：

```text
L1[0]      -> L2_LOW table
L2[32]     -> L3_RAM0 table
L3[0..127] -> 0x04000000 ~ 0x0407ffff
```

注意：

- RAM0 不能使用 L2 block 精确映射。
- 如果使用 L2 block，会映射整个 `0x04000000 ~ 0x041fffff`，即 2MB。

### 5.2 RAM1

RAM1 地址范围：

```text
0x100000000 ~ 0x10007ffff
```

大小：

```text
512KB = 128 * 4KB
```

索引计算：

```text
L1 index = 0x100000000 >> 30 = 4
L2 index = (0x100000000 >> 21) & 0x1ff = 0
L3 index = (0x100000000 >> 12) & 0x1ff = 0
```

页表结构：

```text
L1[4]      -> L2_RAM1 table
L2[0]      -> L3_RAM1 table
L3[0..127] -> 0x100000000 ~ 0x10007ffff
```

注意：

- 不建议使用 L1[4] 直接映射 1GB block。
- 如果 L1[4] 使用 1GB block，会映射 `0x100000000 ~ 0x13fffffff`，远大于 RAM1 的 512KB。

---

## 6. 外设 Device 映射策略

### 6.1 推荐策略

对于 IP 外设，使用 2MB L2 block 映射。

外设区域属性：

```text
Device-nGnRnE
PXN = 1
UXN = 1
```

推荐映射方式：

```text
低地址 IP:
  按 2MB block 单独映射

0x10000000 ~ 0x1fffffff:
  可以整体映射为 Device
```

如果为了简单覆盖 `lynchip-lite-base.md` 中大部分 IP，可以映射：

```text
0x08000000 ~ 0x0fffffff  Device
0x10000000 ~ 0x1fffffff  Device
```

这样可以覆盖：

```text
GIC / Core Timer / CCM
UART / I2C / SPI / GPIO / WDT / TIMER / RTC / DMA / ETH / MMC
PINCTRL / PVT / EFUSE / CPR / EDAC / VPSS / GPU / PCIe
```

但需要额外映射：

```text
0x00000000   Boot ROM / AHB IRAM
0x02000000   SPI1/SFC AHB
0x06000000   IRAM0 Sys IRAM
0x1000000000 APU
```

### 6.2 推荐最终映射表

| VA | PA | Size | Attr |
|---:|---:|---:|---|
| `0x04000000` | `0x04000000` | 512KB | Normal WB |
| `0x100000000` | `0x100000000` | 512KB | Normal WB |
| `0x00000000` | `0x00000000` | 2MB | Device 或 Normal-NC |
| `0x02000000` | `0x02000000` | 2MB | Device |
| `0x06000000` | `0x06000000` | 2MB | Device 或 Normal-WB/NC |
| `0x08000000` | `0x08000000` | 128MB | Device |
| `0x10000000` | `0x10000000` | 256MB | Device |
| `0x1000000000` | `0x1000000000` | 2MB 或按实际大小 | Device |

DDR 不映射。

---

## 7. 页表空间需求

因为 RAM0/RAM1 需要 L3 page table 精确映射，所以页表至少需要：

```text
L1 table        4KB
L2_LOW table    4KB
L2_RAM1 table   4KB
L2_APU table    4KB
L3_RAM0 table   4KB
L3_RAM1 table   4KB
-------------------
Total           24KB
```

建议预留：

```text
32KB
```

不建议继续使用旧设计中的 12KB，因为：

```text
12KB = L1 + L2 + L2
```

不足以支持 RAM0/RAM1 的 L3 精确映射，也不足以优雅支持 APU 高地址映射。

---

## 8. 页表放置要求

页表应放在 `.mmu_table` section，并保证 4KB 对齐。

建议 linker script：

```ld
.mmu_table ALIGN(0x1000) : {
    __mmu_table_start = .;
    KEEP(*(.mmu_table))
    . = ALIGN(0x1000);
    __mmu_table_end = .;
} > IRAM0
```

其中 IRAM0 可用区域建议定义为：

```ld
MEMORY
{
    IRAM0 (rwx) : ORIGIN = 0x04040000, LENGTH = 0x00040000
    IRAM1 (rwx) : ORIGIN = 0x100040000, LENGTH = 0x00040000
}
```

注意：

- `.mmu_table` 本身应位于 `0x04040000 ~ 0x0407ffff`。
- 页表数组必须 4KB 对齐。
- `TTBR0_EL1` 写入的地址也必须 4KB 对齐。
- 页表不能跨越未映射或不可访问区域。

---

## 9. MAIR_EL1 设计

建议使用 3 类 memory attribute：

| AttrIndx | 类型 | MAIR encoding |
|---:|---|---:|
| 0 | Normal WB Cacheable | `0xff` |
| 1 | Normal Non-cacheable | `0x44` |
| 2 | Device nGnRnE | `0x00` |

推荐：

```c
#define MAIR_ATTR_NORMAL_WB      0xffUL
#define MAIR_ATTR_NORMAL_NC      0x44UL
#define MAIR_ATTR_DEVICE_nGnRnE  0x00UL

#define MAIR_VALUE \
    ((MAIR_ATTR_NORMAL_WB     << 0)  | \
     (MAIR_ATTR_NORMAL_NC     << 8)  | \
     (MAIR_ATTR_DEVICE_nGnRnE << 16))
```

即：

```text
MAIR_EL1 = 0x000044ff
```

不建议使用旧设计中的：

```text
MAIR_EL1 = 0x00447f
```

---

## 10. Descriptor 属性设计

### 10.1 Descriptor 类型

AArch64 descriptor 低两位：

```text
Invalid descriptor: 0b00
Block descriptor  : 0b01
Table descriptor  : 0b11
Page descriptor   : 0b11
```

推荐宏：

```c
#define DESC_VALID          (1UL << 0)
#define DESC_TABLE_BIT      (1UL << 1)

#define DESC_BLOCK          (DESC_VALID)
#define DESC_TABLE          (DESC_VALID | DESC_TABLE_BIT)
#define DESC_PAGE           (DESC_VALID | DESC_TABLE_BIT)
```

### 10.2 内存属性

```c
#define ATTR_INDEX_NORMAL_WB    (0UL << 2)
#define ATTR_INDEX_NORMAL_NC    (1UL << 2)
#define ATTR_INDEX_DEVICE       (2UL << 2)

#define DESC_AP_RW_EL1          (0UL << 6)
#define DESC_SH_NON             (0UL << 8)
#define DESC_SH_OUTER           (2UL << 8)
#define DESC_SH_INNER           (3UL << 8)
#define DESC_AF                 (1UL << 10)
#define DESC_PXN                (1UL << 53)
#define DESC_UXN                (1UL << 54)

#define ATTR_NORMAL_WB \
    (ATTR_INDEX_NORMAL_WB | DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF)

#define ATTR_NORMAL_NC \
    (ATTR_INDEX_NORMAL_NC | DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF)

#define ATTR_DEVICE \
    (ATTR_INDEX_DEVICE | DESC_AP_RW_EL1 | DESC_SH_OUTER | DESC_AF | DESC_PXN | DESC_UXN)
```

### 10.3 地址掩码

```c
#define ADDR_MASK_TABLE     0x0000fffffffff000UL
#define ADDR_MASK_L2_BLOCK  0x0000ffffffe00000UL
#define ADDR_MASK_L3_PAGE   0x0000fffffffff000UL
```

生成 descriptor：

```c
static inline uint64_t table_desc(uint64_t addr)
{
    return (addr & ADDR_MASK_TABLE) | DESC_TABLE;
}

static inline uint64_t block_desc(uint64_t addr, uint64_t attr)
{
    return (addr & ADDR_MASK_L2_BLOCK) | attr | DESC_BLOCK;
}

static inline uint64_t page_desc(uint64_t addr, uint64_t attr)
{
    return (addr & ADDR_MASK_L3_PAGE) | attr | DESC_PAGE;
}
```

---

## 11. TCR_EL1 配置

推荐配置：

```text
T0SZ = 25
TG0  = 4KB
SH0  = Inner Shareable
ORGN0/IRGN0 = Normal WB Cacheable
IPS  = 40-bit PA
```

示例：

```c
uint64_t tcr = 0;

tcr |= 25UL;             /* T0SZ: 39-bit VA */
tcr |= (1UL << 8);       /* IRGN0: WB WA RA */
tcr |= (1UL << 10);      /* ORGN0: WB WA RA */
tcr |= (3UL << 12);      /* SH0: Inner Shareable */
tcr |= (2UL << 32);      /* IPS: 40-bit PA */
```

说明：

- RAM1 位于 `0x100000000`，即 4GB。
- APU 位于 `0x1000000000`，即 64GB。
- 因此 IPS 至少不能小于 37-bit。
- 实际建议使用 IPS = 2，即 40-bit PA。

---

## 12. 建表示例结构

页表数组：

```c
static uint64_t l1_table[512]
    __attribute__((aligned(4096), section(".mmu_table")));

static uint64_t l2_low_table[512]
    __attribute__((aligned(4096), section(".mmu_table")));

static uint64_t l2_ram1_table[512]
    __attribute__((aligned(4096), section(".mmu_table")));

static uint64_t l2_apu_table[512]
    __attribute__((aligned(4096), section(".mmu_table")));

static uint64_t l3_ram0_table[512]
    __attribute__((aligned(4096), section(".mmu_table")));

static uint64_t l3_ram1_table[512]
    __attribute__((aligned(4096), section(".mmu_table")));
```

核心映射逻辑：

```c
l1_table[0]  = table_desc((uint64_t)l2_low_table);
l1_table[4]  = table_desc((uint64_t)l2_ram1_table);
l1_table[64] = table_desc((uint64_t)l2_apu_table);

/* RAM0: 0x04000000 ~ 0x0407ffff */
map_l3_page_range(l2_low_table,
                  l3_ram0_table,
                  0x04000000UL,
                  0x04000000UL,
                  0x00080000UL,
                  ATTR_NORMAL_WB);

/* RAM1: 0x100000000 ~ 0x10007ffff */
map_l3_page_range(l2_ram1_table,
                  l3_ram1_table,
                  0x100000000UL,
                  0x100000000UL,
                  0x00080000UL,
                  ATTR_NORMAL_WB);

/* Boot ROM / AHB IRAM */
map_l2_block_range(l2_low_table,
                   0x00000000UL,
                   0x00000000UL,
                   0x00200000UL,
                   ATTR_DEVICE);

/* SPI1/SFC AHB */
map_l2_block_range(l2_low_table,
                   0x02000000UL,
                   0x02000000UL,
                   0x00200000UL,
                   ATTR_DEVICE);

/* IRAM0 Sys IRAM */
map_l2_block_range(l2_low_table,
                   0x06000000UL,
                   0x06000000UL,
                   0x00200000UL,
                   ATTR_DEVICE);

/* GIC/CoreTimer/CCM window */
map_l2_block_range(l2_low_table,
                   0x08000000UL,
                   0x08000000UL,
                   0x08000000UL,
                   ATTR_DEVICE);

/* Peripheral window */
map_l2_block_range(l2_low_table,
                   0x10000000UL,
                   0x10000000UL,
                   0x10000000UL,
                   ATTR_DEVICE);

/* APU */
map_l2_block_range(l2_apu_table,
                   0x1000000000UL,
                   0x1000000000UL,
                   0x00200000UL,
                   ATTR_DEVICE);
```

---

## 13. MMU 启用流程

推荐流程：

```text
1. 关闭或保持 cache disabled
2. 清零页表
3. 构建 L1/L2/L3 页表
4. 写 MAIR_EL1
5. 写 TCR_EL1
6. 写 TTBR0_EL1
7. invalidate TLB
8. DSB/ISB
9. 设置 SCTLR_EL1.M 启用 MMU
10. ISB
11. 确认 MMU 正常后，再启用 I-cache/D-cache
```

示例：

```asm
/* MAIR_EL1 */
ldr     x0, =0x000044ff
msr     mair_el1, x0
isb

/* TCR_EL1 */
ldr     x0, =tcr_config
msr     tcr_el1, x0
isb

/* TTBR0_EL1 */
ldr     x0, =l1_table
msr     ttbr0_el1, x0
isb

/* TLB invalidate */
tlbi    vmalle1
dsb     sy
isb

/* Enable MMU */
mrs     x0, sctlr_el1
orr     x0, x0, #0x1
msr     sctlr_el1, x0
isb
```

Cache 可在 MMU 验证通过后再启用：

```asm
mrs     x0, sctlr_el1
orr     x0, x0, #0x4        /* D-cache */
orr     x0, x0, #0x1000     /* I-cache */
msr     sctlr_el1, x0
isb
```
