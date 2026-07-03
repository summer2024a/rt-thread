# HP232X GIC中断组别配置说明

## 问题背景

根据调试发现，HP232X系统存在中断组别配置问题：

- **Timer中断（IRQ 30）**: 配置在Group0 Secure，Non-secure EL1无法处理
- **UART中断（IRQ 57）**: 可能也存在组别配置问题

在GICv3架构中，中断分为多个组别：
- **Group0**: Secure中断（只能由Secure世界处理）
- **Group1 Secure**: Secure Group1中断
- **Group1 Non-Secure (NS)**: Non-Secure中断（Non-secure EL1可以处理）

## 解决方案

在Non-secure EL1启动后，重新配置Timer和UART中断组别为Group1 NS。

### 代码实现

创建了两个文件：

1. **gic_group_config.c**: 中断组别配置实现
2. **gic_group_config.h**: 中断组别配置头文件

### 配置流程

在`rt_hw_board_init()`中，`rt_hw_gtimer_init()`之前调用：

```c
/* 配置Timer和UART中断组别为Group1 NS */
hp232x_init_interrupt_groups();

/* 验证中断组别配置 */
hp232x_verify_interrupt_groups();
```

## Timer中断配置（IRQ 30）

Timer30是PPI（Private Peripheral Interrupt），需要在GIC Redistributor配置。

### 关键寄存器

- **GICR_IGROUPR0**: SGI/PPI组别寄存器（IRQ 0-31）
  - 地址：`Redistributor_base + 0x10000 (SGI_base) + 0x80`
  - Timer30在bit30，设置为1表示Group1

- **GICR_IGRPMODR0**: SGI/PPI模式寄存器（IRQ 0-31）
  - 地址：`Redistributor_base + 0x10000 (SGI_base) + 0xD00`
  - Timer30在bit30，设置为0表示Non-Secure

### 配置代码

```c
/* 设置Timer30为Group1 */
*gicr_igroupr0 |= (1 << 30);

/* 设置Timer30为Non-Secure */
*gicr_igrpmodr0 &= ~(1 << 30);
```

## UART中断配置（IRQ 57）

UART57是SPI（Shared Peripheral Interrupt），需要在GIC Distributor配置。

### 关键寄存器

- **GICD_IGROUPR**: SPI组别寄存器（IRQ 32+）
  - UART57在GICD_IGROUPR1（寄存器编号 = 57 / 32 = 1）
  - 位索引 = 57 % 32 = 25
  - 地址：`Distributor_base + 0x80 + 1*4`

- **GICD_IGRPMODR**: SPI模式寄存器（IRQ 32+）
  - UART57在GICD_IGRPMODR1
  - 位索引 = 25
  - 地址：`Distributor_base + 0xD00 + 1*4`

### 配置代码

```c
/* 设置UART57为Group1 */
*gicd_igroupr1 |= (1 << 25);

/* 设置UART57为Non-Secure */
*gicd_igrpmodr1 &= ~(1 << 25);
```

## GICv3组别配置原理

### 组别映射规则

| IGROUP bit | IGRPMOD bit | 中断组别 | 处理权限 |
|-----------|-------------|---------|---------|
| 0         | 0           | Group0  | Secure only |
| 0         | 1           | Group1 Secure | Secure only |
| 1         | 0           | Group1 NS | Non-secure OK ✓ |
| 1         | 1           | Group0  | Secure only |

**目标配置**：`IGROUP=1, IGRPMOD=0` → Group1 NS

### Secure vs Non-Secure视图

GICv3在Secure和Non-Secure世界有不同的寄存器视图：

- **EL3（Secure世界）**: 可以访问完整GIC配置
- **EL1 Non-Secure**: 只能访问Non-Secure GIC寄存器

**pre_entry.S在EL3配置**：
```assembly
str     w5, [x3, #0x80]     /* GICR_IGROUP0 = 0xffffffff (all Group1) */
str     wzr, [x3, #0xD00]   /* GICR_IGRPMOD0 = 0 (Non-secure) */
```

但这些配置在Secure世界进行，Non-secure EL1启动后可能看到不同视图。

**需要在Non-secure EL1重新验证和配置**。

## 验证方法

### 寄存器读取验证

通过`hp232x_verify_interrupt_groups()`函数验证：

```c
/* Timer30验证 */
timer_group_bit = (*gicr_igroupr0 >> 30) & 1;  /* 应为1 */
timer_mod_bit = (*gicr_igrpmodr0 >> 30) & 1;   /* 应为0 */

/* UART57验证 */
uart_group_bit = (*gicd_igroupr1 >> 25) & 1;   /* 应为1 */
uart_mod_bit = (*gicd_igrpmodr1 >> 25) & 1;    /* 应为0 */
```

### 启动输出验证

期望看到以下调试输出：

```
========================================
HP232X Interrupt Group Configuration
========================================
[GIC_GROUP] GIC Distributor base: 0x8000000
[GIC_GROUP] GIC Redistributor base for CPU0: 0x8100000
[GIC_GROUP] Configuring Timer IRQ30 group...
[GIC_GROUP] GICR_IGROUPR0 current=0x..., Timer30 bit=X
[GIC_GROUP] GICR_IGROUPR0 new=0x..., Timer30 bit=1 (expect=1) ✓
[GIC_GROUP] GICR_IGRPMODR0 current=0x..., Timer30 bit=X
[GIC_GROUP] GICR_IGRPMODR0 new=0x..., Timer30 bit=0 (expect=0) ✓
[GIC_GROUP] Timer IRQ30 configured to Group1 NS ✓

[GIC_GROUP] Configuring UART IRQ57 group...
[GIC_GROUP] GICD_IGROUPR1 current=0x..., UART57 bit=X
[GIC_GROUP] GICD_IGROUPR1 new=0x..., UART57 bit=1 (expect=1) ✓
[GIC_GROUP] GICD_IGRPMODR1 current=0x..., UART57 bit=X
[GIC_GROUP] GICD_IGRPMODR1 new=0x..., UART57 bit=0 (expect=0) ✓
[GIC_GROUP] UART IRQ57 configured to Group1 NS ✓
```

## 调试要点

### 1. 检查GIC基地址

```c
rt_uint64_t dist_base = platform_get_gic_dist_base();     // 0x8000000
rt_uint64_t redist_base = platform_get_gic_redist_base(); // 0x8100000
```

### 2. SMP系统Redistributor地址计算

每个CPU有自己的Redistributor（128KB大小）：

```c
#ifdef RT_USING_SMP
cpu_id = rt_hw_cpu_id();
redist_base += cpu_id * (2 * 0x10000);  // 128KB per CPU
#endif
```

### 3. 内存屏障

配置后必须使用内存屏障：

```c
__asm__ volatile("dsb sy");
__asm__ volatile("isb");
```

### 4. 中断启用顺序

正确的启用顺序：

1. 配置中断组别（Group1 NS）
2. 配置中断优先级
3. 启用中断（GICR_ISENABLER0 / GICD_ISENABLER）
4. 启用CPU接口（ICC_IGRPEN1_EL1）
5. 启用Timer硬件

## 测试方法

### 自动化测试

```bash
python3 test_interrupt_group.py
```

测试脚本会：
1. SSH连接测试服务器
2. 复位设备
3. 捕获串口输出
4. 分析中断组别配置
5. 验证Timer中断触发

### 手动验证

```bash
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build && scons -j$(nproc)
python3 mkimage.py rtthread.bin rtthread-header.bin
python3 remote_test.py
```

检查启动输出中的中断组别配置标记。

## 相关文档

- [HANDOFF_SLIM.md](HANDOFF_SLIM.md) - 项目进展和问题分析
- [doc/gic_debug.md](doc/gic_debug.md) - GICv3调试要点
- ARM GICv3 Architecture Specification - 官方规范

## 技术要点总结

1. **Timer30和UART57必须配置为Group1 NS**
2. **配置必须在Non-secure EL1层面进行**
3. **pre_entry.S的EL3配置可能不生效或显示不同视图**
4. **验证配置是否生效至关重要**
5. **内存屏障确保配置生效**

## 预期结果

配置完成后：

- ✅ Timer中断正常触发（tick_increase）
- ✅ UART中断在GIC层面启用
- ✅ Non-secure EL1可以处理中断
- ✅ 系统正常运行（Shell、线程调度）

---

**创建日期**: 2026-06-27
**作者**: lynxi
**版本**: 1.0