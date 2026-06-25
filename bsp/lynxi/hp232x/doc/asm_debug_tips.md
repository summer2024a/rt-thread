# HP232X汇编调试技巧

## UART调试输出

### early_putc宏使用
```assembly
#ifdef BSP_USING_HP232X_DEBUG_UART
    /* 初始化UART寄存器 */
    ldr     x6, =0x10006014         /* UART0 LSR address */
    ldr     x7, =0x10006000         /* UART0 THR address */

    /* 使用宏输出 */
    early_putc 'A'                  /* Print 'A' marker */
#endif
```

### 关键要点
- **x6/x7寄存器专用**：仅用于UART调试，不要在其他代码使用
- **宏保护**：所有UART输出必须用`#ifdef BSP_USING_HP232X_DEBUG_UART`保护
- **空实现机制**：禁用DEBUG_UART时，early_putc变成空操作（不触发异常）

## 单字符调试标记

### 标记设计原则
- 使用ASCII字符（易于识别）
- 每个标记代表一个关键检查点
- 按字母顺序或功能分类

### 示例标记序列
```
E3SCGIDXNARBMN234NSPE12MCIBK
  E: CurrentEL检测
  3: EL=3确认
  S: SCR_EL3配置前
  C: SCR_EL3配置后
  G: GIC初始化开始
  I: ICC_SRE_EL3配置后
  D: GIC Distributor配置前
  X: GICD_CTLR写入前
  N: GICD_CTLR写入后
  A: Distributor配置完成
  R: Redistributor配置前
  B: Redistributor base加载
  M: mvn w5, wzr初始化
  ...
```

## 寄存器调试技巧

### 1. 打印寄存器值（简化版）
```assembly
#ifdef BSP_USING_HP232X_DEBUG_UART
    /* 打印寄存器低4位（十六进制） */
    mrs     x0, CurrentEL
    and     x0, x0, #0xF
    cmp     x0, #0xA
    b.ge    hex_high
    add     w8, w0, #'0'           /* 0-9 */
    b       print_reg
hex_high:
    add     w8, w0, #'A'-10        /* A-F */
print_reg:
    early_putc w8
#endif
```

### 2. 验证寄存器写入
```assembly
/* 写入SCR_EL3 */
msr     scr_el3, x0
isb

/* 读回验证 */
mrs     x1, scr_el3
cmp     x0, x1
b.ne    scr_error                /* 写入失败 */
```

## 异常向量表调试

### 1. 简化异常处理程序
```assembly
hp232x_exception_handler_el3:
#ifdef BSP_USING_HP232X_DEBUG_UART
    /* 输出'?'表示异常 */
    adr     x6, exc_simple_uart
    ldr     x6, [x6]
    mov     w8, #'?'
    str     w8, [x6]               /* 直接写入THR，不等待 */
#endif
exc_halt:
    wfe
    b       exc_halt               /* 无限循环 */
```

### 2. 异常向量表对齐
```assembly
.section .hp232x_vectors, "ax", @progbits
.align 11                   /* 2KB对齐 */

hp232x_exception_vectors_el3:
    /* Current EL with SP0 (0x000-0x7FF) */
    .rept 128
    b hp232x_exception_handler_el3
    .endr
    /* ... 其他区域 ... */
```

## 栈调试技巧

### 1. 栈设置验证
```assembly
#ifdef BSP_USING_HP232X
    /* 设置临时栈 */
    ldr     x20, =hp232x_temp_stack_top
    mov     sp, x20
    isb

    /* 验证栈可访问 */
    mov     x0, #0x12345678
    str     x0, [sp, #-8]!         /* Push */
    ldr     x1, [sp], #8           /* Pop */
    cmp     x0, x1
    b.ne    stack_error            /* 栈访问失败 */
#endif
```

### 2. 栈溢出检测
```assembly
/* 在栈底写入魔数 */
ldr     x0, =hp232x_temp_stack
ldr     w1, =0xDEADBEEF           /* Magic number */
str     w1, [x0]

/* 定期检查魔数 */
ldr     w1, [x0]
ldr     w2, =0xDEADBEEF
cmp     w1, w2
b.ne    stack_overflow            /* 栈溢出！ */
```

## 函数调用调试

### 1. 函数调用追踪
```assembly
#ifdef BSP_USING_HP232X_DEBUG_UART
    early_putc 'B'                  /* Before function call */
#endif

    bl      hp232x_bootwrapper_init

#ifdef BSP_USING_HP232X_DEBUG_UART
    early_putc 'A'                  /* After function call */
#endif
```

### 2. 返回地址验证
```assembly
/* 保存返回地址 */
mov     x30_saved, x30

/* 调用函数 */
bl      some_function

/* 验证返回地址 */
mov     x30, x30_saved
ret
```

## 内存访问调试

### 1. IRAM1访问验证
```assembly
/* 测试IRAM1 @ 4GB */
ldr     x0, =0x100000000
ldr     w1, [x0]                   /* Read from IRAM1 */
#ifdef BSP_USING_HP232X_DEBUG_UART
    early_putc 'R'                  /* Read success */
#endif
```

### 2. 页表访问验证
```assembly
/* 测试页表访问 */
ldr     x0, =pgd_base
ldr     x1, [x0]                   /* Read PGD[0] */
#ifdef BSP_USING_HP232X_DEBUG_UART
    early_putc 'P'                  /* Page table accessible */
#endif
```

## 常见调试错误

### 1. UART输出无字符
**原因**：UART寄存器地址错误或未初始化
**解决**：
```assembly
ldr     x6, =0x10006014         /* LSR */
ldr     x7, =0x10006000         /* THR */
```

### 2. UART输出死循环
**原因**：LSR THRE位检查错误
**解决**：
```assembly
1:  ldr     w8, [x6]              /* Read LSR */
    tst     w8, #0x20             /* Check THRE (bit 5) */
    b.eq    1b                    /* Wait if NOT ready */
```

### 3. 寄存器值打印错误
**原因**：十六进制转换错误
**解决**：使用正确转换逻辑（0-9 → '0'-'9', 10-15 → 'A'-'F'）

### 4. 异常向量表不对齐
**原因**：.align参数错误
**解决**：`.align 11`（2KB对齐，不是`.align 2048`）

## 寄存器资源管理

### 专用寄存器
- **x6/x7**：UART调试专用（early_putc依赖）
- **x30**：函数返回地址（link register）
- **x19-x28**：函数调用保存寄存器（platform变量）

### 临时寄存器
- **x0-x18**：函数参数/临时使用
- **x8-x9**：宏内临时寄存器

### 避免冲突
```assembly
/* ❌ 错误：使用x6作为通用寄存器 */
mov     w6, #2
mvn     w6, w6                    /* 破坏x6 UART地址！ */

/* ✅ 正确：使用其他寄存器 */
mov     w9, #2
mvn     w9, w9                    /* 不破坏x6 */
```

## 参考文档
- ARMv8-A Instruction Set Architecture
- UART调试：hp232x-uart-refactoring-complete.md
- 异常向量：pre_entry.S实现