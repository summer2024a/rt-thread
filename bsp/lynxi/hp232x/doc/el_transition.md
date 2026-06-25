# HP232X EL降级流程要点

## EL级别概述

### ARMv8-A Exception Levels
```
EL3: Secure monitor (最高权限)
EL2: Hypervisor (虚拟化)
EL1: OS kernel (操作系统)
EL0: Application (应用程序)
```

### HP232X启动流程
```
BL1 Boot ROM → EL3 (Secure)
pre_entry.S → EL3初始化 + EL降级
entry_point.S → EL1 (Non-secure)
RT-Thread → EL1 kernel
```

## EL3→EL2→EL1降级流程

### Phase 1: EL3配置（pre_entry.S）

**1. SCR_EL3配置**
```assembly
mov     x0, #0x30               /* RES1 bits */
orr     x0, x0, #(1 << 0)       /* NS=1: Next EL in Non-secure */
orr     x0, x0, #(1 << 8)       /* HVC enable */
orr     x0, x0, #(1 << 10)      /* RW=1: 64-bit EL2/EL1 */
msr     scr_el3, x0
isb
```

**2. SCTLR_EL2配置**
```assembly
ldr     x0, =(3 << 28 | 3 << 22 | 1 << 18 | 1 << 16 | 1 << 11 | 3 << 4)
msr     sctlr_el2, x0
isb
```

**3. EL3→EL2降级**
```assembly
adr     x0, .hp232x_after_eret   /* EL2 entry point */
mov     x1, #(9 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))
msr     elr_el3, x0
msr     spsr_el3, x1             /* SPSR: EL2h + exceptions masked */
eret                              /* EL3 → EL2 */
```

### Phase 2: EL2配置（pre_entry.S）

**1. HCR_EL2配置**
```assembly
mov     x0, #(1 << 31)          /* RW=1: 64-bit EL1 */
msr     hcr_el2, x0
isb
```

**2. SCTLR_EL1配置**
```assembly
mov     x0, #0                  /* Disable MMU/Cache at EL1 */
msr     sctlr_el1, x0
isb
```

**3. EL2→EL1降级**
```assembly
adr     x0, .hp232x_el1_continue  /* EL1 entry point */
mov     x1, #(5 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))
msr     elr_el2, x0
msr     spsr_el2, x1             /* SPSR: EL1h + exceptions masked */
eret                              /* EL2 → EL1 */
```

## SPSR_EL3/EL2格式

### SPSR字段
```
M[4:0]: Mode (EL2h=9, EL1h=5)
A[6]: SError interrupt masked
I[7]: IRQ interrupt masked
F[8]: FIQ interrupt masked
D[9]: Debug exception masked
```

### 示例
```assembly
// EL2h + all exceptions masked
mov     x1, #(9 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))
  = 0b0000010011111111 = 0x9FF

// EL1h + all exceptions masked
mov     x1, #(5 | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))
  = 0b0000010111111111 = 0xBFF
```

## CurrentEL检测

### 检测当前EL级别
```assembly
mrs     x0, CurrentEL
lsr     x0, x0, #2              /* CurrentEL[3:2] → EL number */
and     x0, x0, #3              /* EL = 0, 1, 2, or 3 */
```

### EL级别判断
```assembly
cmp     x0, #3
b.eq    el3_init                /* EL3: Full initialization */
cmp     x0, #2
b.eq    el2_init                /* EL2: Skip EL3 init */
cmp     x0, #1
b.eq    el1_boot                /* EL1: Direct boot */
```

## 异常向量表设置

### EL3异常向量表
```assembly
adr     x0, hp232x_exception_vectors_el3
msr     vbar_el3, x0
isb
```

### 异常向量表对齐
- 必须2KB对齐（.align 11）
- 4个区域，每个512字节（128 entries × 4 bytes）

## 调试要点

### 1. 确认BL1跳转级别
```assembly
#ifdef BSP_USING_HP232X_DEBUG_UART
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    add     w8, w0, #'0'
    early_putc w8                 /* Print '3' if EL3 */
#endif
```

### 2. 检查SCR_EL3配置
```assembly
mov     x0, #0x30                /* RES1 bits */
orr     x0, x0, #(1 << 0)        /* NS=1 */
orr     x0, x0, #(1 << 10)       /* RW=1 */
msr     scr_el3, x0
isb
```

**关键位**：
- NS=1：下一级EL在Non-secure状态
- RW=1：下一级EL使用AArch64

### 3. 验证eret跳转
```assembly
adr     x0, .target              /* Target address */
msr     elr_el3, x0
msr     spsr_el3, x1
eret                              /* Jump to target */
```

## 常见问题

### 1. eret后系统挂起
**原因**：SCR_EL3配置错误或目标地址不正确
**解决**：检查NS位和RW位，确认elr_el3地址

### 2. EL降级后触发异常
**原因**：SPSR_EL3模式位错误
**解决**：SPSR M[4:0]必须为目标EL模式（EL2h=9, EL1h=5）

### 3. CurrentEL检测失败
**原因**：CurrentEL[3:2]是EL级别，需要右移2位
**解决**：`lsr x0, x0, #2`后再判断

## 调试标记解读

**pre_entry.S输出**：`E3SC`

| 标记 | 含义 | 检查点 |
|------|------|--------|
| E | CurrentEL检测开始 | EL级别确认 |
| 3 | CurrentEL=3 | BL1跳转到EL3 ✅ |
| S | SCR_EL3配置前 | EL3安全配置准备 |
| C | SCR_EL3配置后 | EL3→EL2降级准备 |

## 参考文档
- ARMv8-A Architecture Reference Manual (Exception Levels)
- lynxi-bootwrapper/boot.S
- ARCHITECTURE_REFACTORING_PLAN.md（架构重构）