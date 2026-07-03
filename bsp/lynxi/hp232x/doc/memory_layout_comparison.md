# HP640 vs HP232X Memory Layout 对比分析

## 硬件相同点

HP640 和 HP232X 是同一款芯片（KA200），共享相同的内存架构：

| 项目 | 值 |
|------|-----|
| 芯片型号 | HP232X / KA200 |
| IRAM0 | 512KB @ 0x04000000 |
| IRAM1 | 512MB @ 0x100000000 |
| 栈地址 | 0x10007FFFC（IRAM1 顶部） |
| BSS 地址 | 0x100040000（IRAM1 起始） |
| MMU | ARMv8-A 4KB granule, 3-level page table |
| TCR_EL1 | IPS=2 (40-bit PA) |
| 启动方式 | BootROM → 两段式 IRAM |

## 内存约束

两款芯片都遵循相同的两段式 IRAM 约束：

| 区域 | 地址范围 | 用途 |
|------|----------|------|
| IRAM0 前256KB | 0x04000000-0x0403FFFF | 保留（bootcode/SPL） |
| IRAM0 后256KB | 0x04040000-0x0407FFFF | 可用（kernel text/data） |
| IRAM1 前256KB | 0x100000000-0x10003FFFF | 保留（不可使用） |
| IRAM1 后256KB | 0x100040000-0x10007FFFF | 可用（BSS/heap/stack） |

## HP640 SPL 布局

### 链接配置
- **SPL_TEXT_BASE**: 0x4040020（IRAM0 后256KB起始）
- **SPL_STACK**: 0x10007FFFC（IRAM1 顶部）
- **SPL_BSS_START**: 0x100040000（IRAM1 起始）
- **SPL_MAX_SIZE**: 256KB（0x40000）
- **SPL_BSS_MAX_SIZE**: 256KB（0x40000）

### 段分布
| Section | 地址 | 大小 | 存储 |
|---------|------|------|------|
| .text | 0x4040020 | 171.5 KB | SRAM |
| .rodata | 0x4069e10 | 49.6 KB | SRAM |
| .dtb.init.rodata | 0x4076050 | 4.7 KB | SRAM |
| .data | 0x40772f0 | 3.7 KB | SRAM |
| .u_boot_list | 0x40781e8 | 6.1 KB | SRAM |
| **.sram 合计** | | **230.5 KB** | **SRAM 256KB** |
| .bss (NOLOAD) | 0x100040000 | 68.0 KB | DDR |
| **栈** | 0x10007FFFC | | DDR顶部 |

### 启动流程
```
BootROM (0x04000000)
  ↓ 加载 SPL 到 0x4040020
SPL (u-boot-spl.bin)
  → lowlevel_init (设置 sp=0x10007FFFC)
  → _main (crt0.S)
  → board_init_f (空实现)
  → spl_board_init
  → spl_load_image (从 MMC 加载 main U-Boot)
  → jump_to_image_no_args (跳转到 0x848000000)
Main U-Boot (0x848000000, 需要 MMU)
  → booti 0x800080000 - 0x843000000 (启动 Linux)
```

## HP232x RT-Thread 布局

### 链接配置
- **IRAM0_KERNEL_BASE**: 0x04040000（BL3_BOOT 模式）
- **KERNEL_VADDR_START**: 0x04040020
- **IRAM1_STACK_TOP**: 0x10007FFFC
- **PAGE_POOL_SIZE**: 16KB
- **HEAP_POOL_SIZE**: 32KB

### 段分布（BL3_BOOT 模式）
| Section | 地址 | 大小 | 存储 |
|---------|------|------|------|
| .head | 0x04040020 | 0.7 KB | IRAM0 |
| .text | 0x04040800 | 168.3 KB | IRAM0 |
| .early_hp232x | 0x0406C000 | 36.0 KB | IRAM0 |
| .data | 0x04075070 | 3.6 KB | IRAM0 |
| .mmu_table | 0x04076000 | 12.0 KB | IRAM0 |
| **IRAM0 合计** | | **~220.6 KB** | **IRAM0 256KB** |
| .bss | 0x100050000 | 52.0 KB | IRAM1 |
| page pool | 0x10005D000 | 16.0 KB | IRAM1 |
| heap | 0x100061000 | 32.0 KB | IRAM1 |
| **IRAM1 数据区** | | **100.0 KB** | |
| early stack | 0x100040000-0x100050000 | 64.0 KB | IRAM1 (entry_point.S 用) |
| main/idle stack | 0x100069000-0x10007FFFC | 92.0 KB | IRAM1 (main/idle thread) |
| **IRAM1 总栈** | | **156.0 KB** | **两段不连续** |
| **IRAM1 总计** | | **256.0 KB** | **IRAM1 256KB** |

> **注意**：IRAM1 栈分为两段不连续区域：
> - Early stack（entry_point.S → hp232x_bootwrapper_init → rt_hw_board_init 前半段）：64KB
> - Main/idle stack（rt_hw_board_init 末尾切换到 0x10007FFFC 后）：92KB
> - BSS/Page/Heap 占据中间 100KB，将两段栈隔开

### 启动流程
```
BootROM (0x04000000)
  ↓ 加载 RT-Thread 到 0x04040020
bootwrapper (pre_entry.S)
  → EL3: GICv3 初始化
  → EL2: HCR_EL2.IMO=0 (关键修复)
  → EL1: 返回 entry_point.S
  → sp = 0x100050000 (IRAM1 early stack, 64KB)
  → hp232x_bootwrapper_init()
  → rtthread_startup()
    → rt_hw_board_init()
      → MMU 配置 + 启用
      → BSS 清零
      → 切换 sp = 0x10007FFFC (IRAM1 main stack, 92KB)
    → rt_show_version()
    → rt_system_timer_init()
    → rt_system_scheduler_init()
    → rt_application_init() (创建 main 线程, sp = 0x10007FFFC)
    → rt_system_scheduler_start()
```

## 关键差异

| 项目 | HP640 (SPL) | HP232x (RT-Thread) |
|------|-------------|-------------------|
| 启动链 | BootROM → SPL → main U-Boot → Linux | BootROM → bootwrapper → RT-Thread |
| 代码段位置 | 0x4040020 (IRAM0 后256KB) | 0x04040020 (IRAM0 后256KB) |
| BSS 位置 | 0x100040000 (IRAM1) | 0x100050000 (IRAM1) |
| 栈位置 | 0x10007FFFC (IRAM1) | 0x10007FFFC (IRAM1) |
| BSS 大小 | 68.0 KB / 256 KB | 52.0 KB (IRAM1) |
| 栈大小 | 0x10007FFFC (DDR顶部) | 156KB 分两段 (early 64KB + main 92KB) |
| MMU 启用时机 | SPL 加载 main U-Boot 后 | bootwrapper 直接启动 RT-Thread |
| 后续加载 | SPL → main U-Boot (0x848000000) | 无后续加载 |

## 设计决策

### 为什么 HP232x 不用 SPL？
1. **空间不足**：SPL 代码段 230.5 KB + bootwrapper ~20 KB = 250.5 KB > 256 KB
2. **地址不匹配**：main U-Boot 链接在 0x848000000，超出 IRAM1 范围（最高 0x101FFFFFFF）
3. **无需外部加载**：RT-Thread kernel 直接链接在 IRAM0，bootwrapper 初始化完成后直接启动

### BL2_BOOT vs BL3_BOOT
- **BL2_BOOT**：kernel 链接在 0x04000020，bootcode 直接跳转
- **BL3_BOOT**：kernel 链接在 0x04040020，需要 bootwrapper 提供跳转 stub

## 参考文档
- [HANDOFF_SLIM.md](../HANDOFF_SLIM.md) - HP232x 项目交接文档
- [mmu_debug.md](mmu_debug.md) - MMU 页表配置要点
- [TEST_METHODOLOGY.md](../TEST_METHODOLOGY.md) - 测试方法论
