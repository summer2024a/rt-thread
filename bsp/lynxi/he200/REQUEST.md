# RPMsg-Lite 中断调试问题记录

## 问题时间
2026-03-30

## 问题现象

### 1. 中断风暴（已解决）
**现象**：系统启动后日志刷屏
```
[TMR-IRQ-ERR] Invalid index 3 vec=69
[TMR-IRQ-ERR] Invalid index 2 vec=68
[TMR-IRQ-ERR] Invalid index 3 vec=69
...
```

**根本原因**：中断标志未清除导致无限循环触发

**分析过程**：
1. 初始实现中，`rpmsg_platform_timer_irq` 在函数最后才调用 `rpmsg_timer_irqhandler` 清除中断标志
2. 当参数验证失败时（如 `timer == NULL`），函数直接 `return` 而未清除中断标志
3. CPU 退出 ISR 后立即重新触发同一中断 → **中断风暴**

**解决方案**：
```c
static void rpmsg_platform_timer_irq(int vector, void *param)
{
    struct timer_config_t *timer = param;

    /* ✅ 第一步：立即清除硬件中断标志 */
    if (timer != RT_NULL && timer->handle != RT_NULL) {
        rpmsg_timer_irqhandler(timer->handle);
    }

    /* 第二步：参数验证（此时中断已清除，不会重复触发） */
    if (timer == RT_NULL) {
        LOG_E("NULL param vec=%d", vector);
        return;  /* 安全返回，中断已清除 */
    }

    /* ... 其他验证 ... */
}
```

**关键点**：
- 中断标志清除必须放在 ISR 入口处
- 即使后续验证失败提前返回，中断标志也已被清除
- 防止形成无限循环

---

### 2. 定时器 Index 验证错误（已解决）
**现象**：日志显示 `Invalid index 2` 和 `Invalid index 3`

**根本原因**：验证逻辑混淆了"数组大小"和"成员字段取值范围"

```c
/* ❌ 错误逻辑 */
if (timer->index >= 2) {  // ← 拒绝所有 >=2 的值
    LOG_E("Invalid index %u", timer->index);
    return;
}

/* 实际情况 */
#define RPMSG_PLATFORM_TIMER_TVQ_IDX 2  /* TX 队列使用 index 2 */
#define RPMSG_PLATFORM_TIMER_RVQ_IDX 3  /* RX 队列使用 index 3 */

static struct timer_config_t g_timer_config[2] = {
    {2, ...},  /* index = 2 */
    {3, ...}   /* index = 3 */
};
```

**问题分析**：
- `g_timer_config[2]` 表示数组有 2 个元素（索引 0 和 1）
- 但每个元素的 `.index` 字段值是 2 和 3（不是数组索引！）
- 验证逻辑 `>=2` 错误地拒绝了所有合法的 index 值

**修正方案**：
```c
/* ✅ 正确逻辑：验证是否在合理范围内 */
if (timer->index > 15) {
    LOG_E("Invalid index %u (corrupted)", timer->index);
    return;
}
```

**经验教训**：
- 区分"数组索引"和"硬件资源编号"
- 验证应该检查合理性（如 0-15），而不是与数组大小比较
- 预留扩展空间，便于未来修改

---

### 3. Timer Config 结构体内容破坏（待解决）⚠️
**现象**：日志中出现乱码字符
```
Timer[68~69] `) 0x00000008002916d0, `) (nil)
Timer[68~69] ) 0x0000000800291730, ) (nil)  // ← 出现乱码
```

**关键观察**：
1. **中断确实在触发** - 清除中断标志后不再刷屏
2. **指针变量异常** - `timer->name` 指向非法内存区域
3. **地址格式可疑** - `0x00000008002916d0` 看起来像 32 位截断的 64 位地址

**当前状态**：
- ✅ 中断标志清除逻辑正确
- ✅ Index 验证逻辑正确
- ❌ `timer_config` 结构体内容被破坏

**可能的根本原因**：

#### 原因 1: MMU 映射问题 🔴
- BSS 段可能未正确映射到 DDR 物理地址
- `g_timer_config` 静态全局变量位于 BSS 段
- 如果 MMU 页表配置错误，访问会导致异常

**验证方法**：
```bash
# GDB 调试
aarch64-none-elf-gdb rtthread.elf
(gdb) target remote :3333
(gdb) p &g_timer_config
# 期望输出：0x8002916d0 <g_timer_config> (在 DDR 范围内)
# 如果输出：0x00000008002916d0 (32 位截断) → MMU 配置问题
```

#### 原因 2: 栈溢出导致数据段破坏 🔴
- 中断栈或线程栈溢出可能覆盖相邻的 BSS 段
- `g_timer_config` 位于 BSS 段，可能被破坏

**验证方法**：
```bash
msh />list_thread
# 检查各线程栈使用率
# 如果某线程栈使用率 >90%，可能溢出
```

**解决方案**：
```c
/* rtconfig.h 中增加栈大小 */
#define RT_INTERRUPT_STACK_SIZE 4096  /* 默认可能太小 */
```

#### 原因 3: 多核共享内存布局冲突 🔴
- Host 端和设备端的共享内存布局可能重叠
- Host 端误写设备端全局变量区域

**验证方法**：
- 检查 Host 侧的共享内存地址配置
- 对比 Device 侧的 BSS 段地址范围
- 确认两者无重叠

#### 原因 4: 链接脚本配置错误 🔴
- `linker.x` 中的内存区域定义可能不正确
- BSS 段起始地址计算错误

**验证方法**：
```bash
# 查看 Map 文件
aarch64-none-elf-nm rtthread.elf | grep g_timer_config
# 期望输出：00000008002916d0 B g_timer_config
# 如果地址不对 → 链接脚本问题
```

**下一步调试计划**：

1. **GDB 调试**（如果条件允许）
   ```bash
   aarch64-none-elf-gdb rtthread.elf
   (gdb) target remote :3333
   (gdb) break rpmsg_platform_timer_irq
   (gdb) continue
   
   # 触发中断后
   (gdb) p timer->index
   (gdb) p timer->name
   (gdb) p timer->irq_no
   (gdb) x/20bx timer  # 查看完整结构体内容
   ```

2. **增强日志输出**
   ```c
   /* 打印更多调试信息 */
   LOG_D("timer=%p name_ptr=%p name[0]=%02x", 
         timer, timer->name, timer->name ? timer->name[0] : 0);
   ```

3. **检查 MMU 配置**
   - 查看 `mmu.c` 或相关初始化代码
   - 确认 BSS 段地址范围已正确映射
   - 验证页表项属性（可读/可写/不可执行）

4. **检查链接脚本**
   - 打开 `linker.x` 或相应链接脚本
   - 查找 `.bss` 段定义
   - 确认起始地址和大小符合预期

**临时规避方案**：
```c
#ifdef RPMSG_VQ_DEBUG
    /* 安全的日志输出，避免打印损坏的字符串 */
    if (timer->name != RT_NULL) {
        uintptr_t name_addr = (uintptr_t)timer->name;
        if ((name_addr >= 0x800000000UL) && (name_addr <= 0x9FFFFFFFFUL)) {
            LOG_D("%d (%s) vec=%d", timer->index, timer->name, vector);
        } else {
            LOG_D("%d (<bad-name>) vec=%d", timer->index, vector);
        }
    } else {
        LOG_D("%d (NULL name) vec=%d", timer->index, vector);
    }
#endif
```

---

## 总结与经验教训

### 中断处理设计原则
1. ⭐ **中断标志尽早清除** - 放在 ISR 入口，不要延迟
2. ⭐ **验证逻辑要合理** - 区分数组索引和硬件资源编号
3. ⭐ **使用异步日志** - 避免在中断中使用 `rt_kprintf`
4. ⭐ **指针验证要全面** - 不仅验证 NULL，还要验证地址范围

### AArch64 平台特殊注意事项
1. ⭐ **64 位地址完整性** - 注意 32 位截断问题
2. ⭐ **MMU 恒等映射** - 虚拟地址 = 物理地址（`0x800000000` ~ `0x9FFFFFFFF`）
3. ⭐ **BSS 段位置验证** - 全局变量应在 DDR 范围内
4. ⭐ **多核内存布局** - Host 和 Device 共享内存不能重叠

### 调试方法论
1. **分步排查**：先解决中断风暴，再解决指针异常
2. **日志分级**：ERROR/WARNING/DEBUG 级别分离
3. **安全输出**：字符串指针先验证再打印
4. **工具辅助**：GDB、Map 文件、内存转储综合分析

---

## 相关文件
- `/data/biao.xia/rt-thread/bsp/lynxi/he200/packages/rpmsg-lite-latest/lib/rpmsg_lite/porting/platform/rpmsg_platform.c`
  - `rpmsg_platform_vq_isr()` - Virtqueue 中断处理
  - `rpmsg_platform_timer_irq()` - 定时器中断处理
  - `rpmsg_platform_timer_cb()` - 定时器回调
  - `g_timer_config[]` - 全局定时器配置数组

- `/data/biao.xia/rt-thread/bsp/lynxi/he200/drivers/drv_timer.c`
  - `dw_timer_initialize()` - 定时器初始化
  - `rpmsg_timer_irqhandler()` - 定时器中断清理

- `/data/biao.xia/rt-thread/bsp/lynxi/he200/linker.x` （需要检查）
  - BSS 段地址定义

- `/data/biao.xia/rt-thread/bsp/lynxi/he200/mmio.c` 或 `mmu.c` （需要检查）
  - MMU 页表配置

---

## 参考文档
- [RPMsg-Lite.md](./RPMsg-Lite.md) - RPMsg-Lite 协议说明
- [README.md](./README.md) - HE200 BSP 说明
- RT-Thread 编程指南 - 中断和内存管理章节
