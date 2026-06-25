# HP232X GICv3中断控制器调试要点

## GICv3架构关键理解

### Secure vs Non-Secure视图差异

**这是GICv3的正常行为，不是bug！**

| EL Level | Security State | GICD_CTLR可见位 | 示例值 |
|----------|---------------|----------------|-------|
| EL3 | Secure | 所有位（包括ARE_NS等Secure位） | 0x37 (ARE_NS=1) |
| EL1 | Non-Secure | 只能看到Non-Secure位（看不到ARE_NS） | 0x12 (ARE_NS不可见) |

**示例**：
- EL3写入GICD_CTLR = 0x37（包含ARE_NS=1）
- EL1 NS读取时只能看到Non-Secure位（0x12）
- **结论**：这是架构的正常行为

## GIC初始化流程（bootwrapper风格）

### 参考实现
- lynxi-bootwrapper/gic.S:86-209
- u-boot SPL gic_64.S

### 关键步骤

**1. EL3启用GICv3系统寄存器接口**
```assembly
mov     x0, #((1 << 3) | (1 << 0))  /* Enable=1 | SRE=1 */
mrs     x1, ICC_SRE_EL3              /* S3_6_C12_C12_5 */
orr     x1, x1, x0
msr     ICC_SRE_EL3, x1
isb
```

**2. 配置GIC Distributor（GICD_CTLR = 0x37）**
```assembly
ldr     x1, =0x08000000             /* GIC500 Distributor base */
mov     w0, #7                      /* EnableGrp0 | EnableGrp1ns | EnableGrp1s */
orr     w0, w0, #(3 << 4)           /* ARE_S | ARE_NS */
str     w0, [x1]                    /* GICD_CTLR = 0x37 */
```

**3. 配置Redistributor**
```assembly
ldr     x2, =0x08100000             /* Redistributor base */
mov     w9, #2
mvn     w9, w9                      /* w9 = 0xFFFFFFFD */
ldr     w4, [x2, #0x014]            /* GICR_WAKER */
and     w4, w4, w9                  /* Clear ProcessorSleep */
str     w4, [x2, #0x014]
```

**4. 配置ICC系统寄存器**
```assembly
mov     x10, #0x3                   /* EnableGrp1NS=1 | EnableGrp1S=1 */
msr     ICC_IGRPEN1_EL3, x10        /* S3_6_C12_C12_7 */
isb

mrs     x10, ICC_SRE_EL2            /* S3_4_C12_C9_5 */
orr     x10, x10, #0xf              /* Allow EL1 access */
msr     ICC_SRE_EL2, x10
isb

mov     x10, #0x80                  /* Non-Secure access to PMR */
msr     ICC_PMR_EL1, x10            /* S3_0_C4_C6_0 */
isb
```

## 中断组别配置

### Group 0 vs Group 1
- **Group 0**：Secure中断，触发FIQ
- **Group 1**：Non-secure中断，触发IRQ
- **UART中断必须设置为Group 1 NS**才能被RT-Thread捕获

### SPI中断组别设置
```assembly
ldr     w2, [x1, #4]                /* GICD_TYPER */
and     w2, w2, #0x1f               /* ITLinesNumber */
add     x3, x1, #0x84               /* GICD_IGROUP1 */
add     x4, x1, #0xD04              /* GICD_IGRPMOD1 */
mvn     w5, wzr                     /* w5 = 0xFFFFFFFF */

str     w5, [x3], #4                /* Set Group1 */
str     wzr, [x4], #4               /* Set Non-secure */
```

## 调试标记解读

**pre_entry.S输出**：`???E3SCGIDXNARBMN234NSPE12MCIBK`

| 标记 | 含义 | 检查点 |
|------|------|--------|
| E3 | CurrentEL=3 | EL3级别确认 |
| SC | SCR_EL3配置 | EL3安全配置 |
| GID | GIC Distributor初始化 | GICD_CTLR=0x37 |
| XNARBM | GIC配置标记 | Distributor/Redistributor |
| 234N | Redistributor唤醒 | GICR_WAKER配置 |
| SPE12MC | ICC系统寄存器 | ICC_IGRPEN1/ICC_SRE_EL2/ICC_PMR |

## 常见问题排查

### 1. UART中断触发FIQ而非IRQ
**原因**：UART中断在Group 0（Secure）
**解决**：设置GICD_IGROUP/GICD_IGRPMOD为Group 1 NS

### 2. 无法修改GICD_CTLR.ARE_NS
**原因**：RT-Thread在EL1 NS无权修改Secure寄存器
**解决**：在EL3完成配置（pre_entry.S）

### 3. GICD_CTLR读取值不符预期
**原因**：EL1 NS看不到Secure位
**验证**：EL3读取0x37，EL1读取0x12（正常）

## 寄存器地址

| 寄存器 | 地址 | 说明 |
|--------|------|------|
| GIC500 Distributor | 0x08000000 | 全局中断控制器 |
| GIC500 Redistributor | 0x08100000 | Per-CPU中断控制器 |
| GICD_CTLR | Offset 0x000 | Distributor控制寄存器 |
| GICD_TYPER | Offset 0x004 | 中断类型寄存器 |
| GICR_WAKER | Offset 0x014 | Redistributor唤醒寄存器 |
| ICC_SRE_EL3 | S3_6_C12_C12_5 | EL3系统寄存器接口 |
| ICC_IGRPEN1_EL3 | S3_6_C12_C12_7 | Group 1中断使能 |

## 参考文档
- ARM GICv3 Architecture Specification
- lynxi-bootwrapper/gic.S
- u-boot SPL gic_64.S
- GIC_SECURE_INIT_GUIDE.md（详细实现）