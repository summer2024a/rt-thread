# HP232X内存分配器调试要点

## 内存约束

### IRAM1可用空间
- **前256KB保留**：0x100000000-0x10003FFFF
- **后256KB可用**：0x100040000-0x10007FFFF
- **实际可用heap**：144KB（扣除stack、page pool等）

## SLAB vs Small Memory Allocator

### SLAB Allocator问题

**zone_size计算机制**：
```c
// RT-Thread SLAB算法
zone_size = 1 << (RT_PAGE_MAX_ORDER + ARCH_PAGE_SHIFT - 1)

// 对于RT_PAGE_MAX_ORDER=11, ARCH_PAGE_SHIFT=12:
zone_size = 1 << (11 + 12 - 1) = 1 << 22 = 4MB

// HP232X约束：
heap_size = 144KB < zone_size (4MB)
```

**问题**：
- SLAB allocator要求至少4MB连续空间
- HP232X只有144KB heap，违反SLAB要求
- 创建zone失败 → Shell thread创建失败

### Small Memory Allocator优势

**无zone限制**：
- 直接从heap切割内存块
- 每个内存块仅32字节管理开销
- 完美适配小heap（144KB）

**配置方法**：
```c
// rtconfig.h
#define RT_USING_SMALL_MEM
// #define RT_USING_SLAB          // 禁用SLAB
```

## Heap初始化

### 固定地址初始化
```c
// board.c
#define HEAP_START  0x100068000
#define HEAP_END    0x100074000
#define HEAP_SIZE   0x0C000      // 48KB

rt_system_heap_init((void*)HEAP_START, (void*)HEAP_END);
```

### 内存布局
```
IRAM1 (0x100040000-0x10007FFFF):
  0x100040000-0x1000405FF: CPU stacks (1.5KB)
  0x100041000-0x100047FFF: Page tables (28KB)
  0x100048000-0x100064000: .bss (144KB)
  0x100064000-0x100068000: Page pool (16KB)
  0x100068000-0x100074000: Heap (48KB) ✅
  0x100074000-0x10007FFFC: 空闲空间
```

## malloc测试验证

### 简单测试
```c
void *ptr = rt_malloc(64);
if (ptr != RT_NULL) {
    rt_kprintf("malloc 64 bytes success\n");
    rt_free(ptr);
} else {
    rt_kprintf("malloc failed\n");
}
```

### Heap大小检查
```c
rt_size_t total, used, max;
rt_memory_info(&total, &used, &max);
rt_kprintf("Heap: total=%d, used=%d, max=%d\n", total, used, max);
```

## 调试标记解读

**board.c输出**：`IiMmx`

| 标记 | 含义 | 检查点 |
|------|------|--------|
| Ii | Heap初始化成功 | rt_system_heap_init |
| Mm | malloc 64字节测试 | Small mem工作 |
| x | malloc返回非NULL | 分配成功 |

## 常见问题

### 1. Shell thread创建失败
**症状**：`(tid != RT_NULL) assertion failed`
**原因**：SLAB allocator zone_size=4MB > heap=144KB
**解决**：使用Small Memory Allocator

### 2. malloc返回NULL
**原因**：Heap未初始化或大小不足
**解决**：检查rt_system_heap_init调用和HEAP_SIZE定义

### 3. Heap访问异常
**原因**：Heap地址在IRAM1前256KB保留区
**解决**：确保HEAP_START ≥ 0x100040000

## 配置要点

### rtconfig.h配置
```c
// 内存分配器选择
#define RT_USING_SMALL_MEM        // 使用Small mem（推荐）
// #define RT_USING_SLAB         // 禁用SLAB（不兼容144KB heap）

// Heap大小配置
#define ARCH_HEAP_SIZE 0x0C000    // 48KB（适配IRAM1约束）

// Page pool大小
#define ARCH_INIT_PAGE_SIZE 0x04000  // 16KB
```

### link.lds配置
```ld
/* IRAM1 section */
.bss :
{
    . = ALIGN(8);
    __bss_start = .;
    *(.bss)
    *(COMMON)
    . = ALIGN(8);
    __bss_end = .;
} > IRAM1

/* Heap在IRAM1后256KB */
. = 0x100068000;
__heap_start = .;
. = . + ARCH_HEAP_SIZE;
__heap_end = .;
```

## 参考文档
- RT-Thread Memory Management
- MEMORY_ALLOCATOR_DEBUG.md（详细调试）