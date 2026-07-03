# HP232X Timer/UART中断组别配置完成报告

## 测试日期
2026-06-27

## 问题总结

### 原始问题
Timer30和UART57中断在Group0 Secure，Non-secure EL1无法处理。

### 解决方案

#### 1. 配置GICD_CTLR的DS位（关键）

修改pre_entry.S，在GICD_CTLR配置中添加DS位：

```assembly
/* Enable all groups and affinity routing */
/* CRITICAL: Set DS bit (bit 6) to disable Security state separation */
mov     w0, #7                      /* EnableGrp0 | EnableGrp1ns | EnableGrp1s */
orr     w0, w0, #(3 << 4)           /* ARE_S | ARE_NS */
orr     w0, w0, #(1 << 6)           /* DS=1 (Disable Security) */
str     w0, [x1]                    /* GICD_CTLR = 0x77 */
```

**DS位的作用**：
- DS=1: 禁用安全状态分离，所有中断被视为Non-secure
- DS=0: 保持安全和非安全分离，Non-secure只能访问NS寄存器

**效果**：设置DS=1后，Non-secure EL1可以成功读写GIC组别寄存器。

#### 2. 配置GICR_NSACR寄存器

在pre_entry.S的Redistributor配置中添加NSACR配置：

```assembly
/* Configure GICR after wakeup */
add     x3, x2, #(1 << 16)          /* SGI_base offset */

str     w5, [x3, #0x80]             /* GICR_IGROUP0 = 0xffffffff */
str     wzr, [x3, #0xD00]           /* GICR_IGRPMOD0 = 0 */

/* CRITICAL: Enable NS access to GICR registers */
mov     w11, #0xFFFFFFFF            /* Allow NS access to all SGI/PPI */
str     w11, [x3, #0xE00]           /* GICR_NSACR = 0xffffffff */
dsb     sy
isb
```

**GICR_NSACR的作用**：
- 每个bit控制对应SGI/PPI中断的NS访问权限
- bit=0: NS不能访问该中断的组别寄存器
- bit=1: NS可以访问该中断的组别寄存器

#### 3. 禁用BSP_USING_HP232X_SPIN_TABLE宏

在rtconfig.h中禁用这个宏：

```c
/* Use simple spin table for secondary CPUs */
/* CRITICAL: This macro disables full IRQ handler, causing interrupts to fail */
/* #define BSP_USING_HP232X_SPIN_TABLE */
```

**原因**：该宏导致vector_irq变成死循环，无法处理中断。

```assembly
START_POINT(vector_irq)
#ifdef BSP_USING_HP232X_SPIN_TABLE
    /* Minimal IRQ handler - just loop */
1:
    wfe
    b       1b
#else
    /* Full IRQ handler - save context and call rt_hw_trap_irq */
    ...
#endif
```

#### 4. 在Non-secure EL1验证配置

添加gic_group_config.c模块，在rt_hw_gtimer_init()之前验证组别配置：

```c
void hp232x_init_interrupt_groups(void)
{
    hp232x_config_timer_interrupt_group(redist_base);
    hp232x_config_uart_interrupt_group(dist_base);
}

void hp232x_verify_interrupt_groups(void)
{
    /* 验证Timer30: IGROUPR0 bit30=1, IGRPMODR0 bit30=0 */
    /* 验证UART57: IGROUPR1 bit25=1, IGRPMODR1 bit25=0 */
}
```

## 测试结果

### 成功项 ✓

1. **中断组别配置成功**：
   ```
   [GIC_VERIFY] Timer IRQ30:
     GICR_IGROUPR0: 0xffffffff (bit30=1, expect=1)
     GICR_IGRPMODR0: 0x00000000 (bit30=0, expect=0)
     ✓ Timer configured to Group1 NS (correct)

   [GIC_VERIFY] UART IRQ57:
     GICD_IGROUPR1: 0xffffffff (bit25=1, expect=1)
     GICD_IGRPMODR1: 0x00000000 (bit25=0, expect=0)
     ✓ UART configured to Group1 NS (correct)
   ```

2. **Timer硬件配置正确**：
   ```
   CNTP_CTL_EL0:  0x0000000000000001 (ENABLE=1 IMASK=0)
   CNTP_TVAL_EL0: 0x0000000000039172 (7ms until next tick)
   Timer frequency: 32MHz
   ```

3. **GIC配置正确**：
   ```
   GICD_CTLR:     0x00000052 (EnableGrp1NS=1 ARE_NS=1)
   GICR_ISENABLER0: 0x40000000 (Timer30=Y)
   Timer priority: 0xa0 (160) < PMR 0xf8 (248) ✓
   ICC_IGRPEN1_EL1: 0x0000000000000001 (Enable=1)
   ```

4. **CPU状态正确**：
   ```
   DAIF:          0x0000000000000000 (I=0, interrupts enabled)
   CurrentEL:     EL=1 (Non-secure EL1)
   VBAR_EL1:      0x000000000400E000 (Vector table configured)
   ```

### 未解决问题 ✗

Timer中断仍未触发（System tick=0, ISR_count=0）

**可能原因**：
1. 中断处理流程需要进一步调试
2. 可能需要检查中断路由配置
3. 可能需要验证中断处理函数是否被调用

## 技术要点总结

### GICv3组别配置关键寄存器

| 寄存器 | 地址 | 作用 | 配置值 |
|--------|------|------|--------|
| GICD_CTLR | GICD_base + 0x000 | Distributor控制 | 0x77 (DS=1) |
| GICR_IGROUPR0 | SGI_base + 0x80 | SGI/PPI组别 | 0xffffffff |
| GICR_IGRPMODR0 | SGI_base + 0xD00 | SGI/PPI模式 | 0x00000000 |
| GICR_NSACR | SGI_base + 0xE00 | NS访问权限 | 0xffffffff |

### 组别映射规则

```
IGROUP bit | IGRPMOD bit | 中断组别 | 处理权限
-----------|-------------|---------|----------
0         | 0           | Group0  | Secure only
0         | 1           | Group1 Secure | Secure only
1         | 0           | Group1 NS | Non-secure OK ✓
1         | 1           | Group0  | Secure only
```

### Timer中断配置流程

```
1. EL3阶段配置GICD_CTLR (DS=1)
2. EL3阶段配置GICR_NSACR (允许NS访问)
3. EL3阶段配置GICR_IGROUPR0 (Timer30 → Group1)
4. Non-secure EL1验证组别配置
5. rt_hw_gtimer_init()配置Timer
6. rt_hw_interrupt_install(30, handler)
7. rt_hw_interrupt_umask(30)启用GIC中断
8. rt_hw_gtimer_enable()启用Timer硬件
```

## 下一步调试建议

1. **验证中断处理函数调用**：
   - 在vector_irq添加调试输出
   - 检查SAVE_IRQ_CONTEXT是否正确
   - 验证rt_hw_trap_irq是否被调用

2. **检查中断路由**：
   - 验证GICD_IROUTER配置
   - 确保Timer中断路由到当前CPU

3. **检查ICC系统寄存器**：
   - 验证ICC_BPR1_EL1配置
   - 验证ICC_CTLR_EL1配置

4. **添加中断计数器**：
   - 在rt_hw_timer_isr添加计数器
   - 在vector_irq入口添加计数器
   - 验证中断是否到达CPU

## 文件清单

### 新增文件
- `drivers/gic_group_config.c` - 中断组别配置实现
- `drivers/gic_group_config.h` - 头文件
- `test_interrupt_group.py` - 自动化测试脚本
- `test_quick.sh` - 快速测试脚本
- `doc/interrupt_group_config.md` - 技术文档

### 修改文件
- `drivers/pre_entry.S` - 添加DS位和NSACR配置
- `drivers/board.c` - 调用中断组别配置和验证
- `rtconfig.h` - 禁用SPIN_TABLE宏

## 编译状态

✅ 编译成功
✅ 内存约束满足：IRAM0 202.12 KB (79%), IRAM1 132.00 KB (52%)
✅ 启动镜像生成：208 KB

## 参考资料

- ARM GICv3 Architecture Specification
- [HANDOFF_SLIM.md](HANDOFF_SLIM.md) - 项目进展
- [doc/gic_debug.md](doc/gic_debug.md) - GIC调试要点
- lynxi-bootwrapper gic.S - 参考实现

---

**报告日期**: 2026-06-27
**状态**: 中断组别配置成功，Timer中断触发待调试