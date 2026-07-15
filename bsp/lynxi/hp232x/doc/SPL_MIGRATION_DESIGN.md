# HP232x SPL 模块移植设计文档

## 1. 背景

### 1.1 源端代码

源端代码位于 `hp640_arm/common/spl/`，是基于 **Lynxi KA200/HP640 SoC** 的 SPL（Secondary Program Loader）子系统，运行在 ARMv8-A 大核上。主要模块包括：

| 模块 | 源文件 | 功能 |
|------|--------|------|
| **eMMC 驱动** | spl_cmd.c | 完整的 eMMC 初始化、模式切换（HS200/HS400）、ADMA3 传输、读写、压力测试 |
| **APU 控制** | spl_cmd.c | APU 寄存器读写、程序加载（x_loader）、控制器开关 |
| **Flash (SPI)** | spl_cmd.c | SPI Flash 打开/读/写/擦除，DLL offset 配置存储 |
| **EFUSE** | lx_efuse.c | eFuse UUID 读取、芯片类型检测 |
| **PVT（温度/电压）** | spl_pvt.c | PVT IP 的温度传感器(TS)、电压监测(VM)、工艺探测器(PD) 初始化、采样、计算 |
| **I2C 接口** | spl_cmd.c (i2c_wait_interrupt) | DesignWare I2C 控制器，MCU I2C 从机协议 |
| **日志系统** | spl_log.c, spl_log_buffer.c | 分级日志（ERROR/WARN/INFO/DEBUG/TRACE），RAM 环形缓冲区，错误冻结保护 |
| **CLI 框架** | spl_cli.c | 简单命令行解析与命令分发 |

### 1.2 目标平台

| 项目 | 规格 |
|------|------|
| **SoC** | KA200 (ARMv8-A, Cortex-A53 class, 双核 SMP) |
| **RTOS** | RT-Thread 5.3.0 (SMP) |
| **Memory** | IRAM0 256KB + IRAM1 256KB (无外部 DDR) |
| **UART** | DW APB UART0 @ 0x10006000, 115200 baud |
| **GIC** | GICv3 |
| **Boot** | BL1 → pre_entry.S (EL3) → entry_point.S (EL1) → RT-Thread |

### 1.3 双核架构约束

| 核心 | 职责 | 调度模型 |
|------|------|----------|
| **Core0 (CPU0)** | APU、Flash、EFUSE、PVT、I2C、日志、CLI 命令、中断处理 | RT-Thread 多线程，可响应中断 |
| **Core1 (CPU1)** | eMMC 主循环线程 | 独立线程，轮询模式，**无中断** |

---

## 2. 内存约束

HP232x 的内存极其紧张（IRAM-only，共 512KB）：

| 区域 | 地址 | 大小 | 用途 |
|------|------|------|------|
| **IRAM0** (kernel text/data) | 0x04000000 | 256KB | .head + .text + .data + .mmu_table |
| **IRAM1** (runtime data) | 0x100040000 | 256KB | .bss + heap + page_pool + stack |

### IRAM1 详细布局

```
0x100040000 ┌──────────────────────┐
            │  CPU Stacks + early  │  ~64KB
            │  data                │
0x100050000 ├────────��─────────────┤
            │  .bss section        │  ~52KB
0x10005D000 ├──────────────────────┤
            │  Page Pool           │  16KB
0x100061000 ├──────────────────────┤
            │  Heap (Small Mem)    │  32KB
0x100069000 ├──────────────────────┤
            │  Stack Area (grows↓) │  ~92KB
0x10007FFF0 └──────────────────────┘
```

**关键约束**：
- Heap 仅 32KB（`ARCH_HEAP_SIZE = 0x8000`），使用 Small Mem Allocator
- 所有 SPL 模块的数据必须控制在几十 KB 以内
- **不能使用 `malloc()` 动态分配**（heap 太小且碎片化）
- 所有缓冲区必须静态分配在 .bss 或 .data 段

---

## 3. 模块移植清单

### 3.1 新建文件

```
bsp/lynxi/hp232x/drivers/
├── spl_log.c              # 日志输出
├── spl_log_buffer.c       # 日志缓冲区管理
├── spl_efuse.c            # EFUSE 模块
├── spl_emmc_core1.c       # eMMC Core1 线程（无中断）
├── spl_emmc_api.c         # eMMC 对外 API
├── spl_apu.c              # APU 控制
├── spl_flash.c            # SPI Flash
├── spl_i2c.c              # I2C 从机协议
├── spl_pvt.c              # PVT 模块
└── spl_cmd.c              # SPL 命令集

bsp/lynxi/hp232x/applications/
└── spl_main.c             # SPL 子系统入口（注册线程/初始化）

bsp/lynxi/hp232x/doc/
└── SPL_MIGRATION_DESIGN.md  # 本文档
```

### 3.2 复用现有驱动

| 现有驱动 | 文件 | 对接关系 |
|----------|------|----------|
| UART0 | drv_uart.c | CLI 输入源、日志输出 |
| APB Timer | drv_apb_timer.c | 时间基准 |
| GICv3 | pmon_gic.c | 中断分发 |

---

## 4. 各模块详细设计

### 4.1 SPL 日志系统

#### 4.1.1 关键变更

| hp640 SPL | HP232x RT-Thread |
|-----------|------------------|
| `DECLARE_GLOBAL_DATA_PTR` | 移除 |
| `flush_cache()` / `invalidate_dcache_range()` | 使用 `rt_hw_cpu_dcache_clean_by_mva()` / `rt_hw_cpu_dcache_invalidate_by_mva()` |
| 固定 SRAM `0x100050000`（64位） | IRAM1 静态分配 `.bss` 段 |
| `get_timer()` | `rt_tick_get()` |
| `printf()` | `rt_kprintf()` |

#### 4.1.2 日志缓冲区设计

```c
/* 放在 .bss 段，不占用 IRAM0 代码空间 */
struct spl_log_buffer g_spl_log_buffer __attribute__((section(".bss.spl_log_buf")));

/* 缓冲区大小：32KB（IRAM1 的 1/8） */
#define SPL_LOG_BUFFER_SIZE   (32 * 1024)
#define SPL_LOG_ENTRY_MAX_LEN  256
```

> **为什么比裸机版本大**：IRAM1 有 256KB 可用（扣除 stack/bss/page_pool/heap 后），32KB 足够容纳大量启动日志。

### 4.2 SPL 错误码

直接从 `spl_cmd.c` 提取 `enum SPL_ERR_CODE`。

### 4.3 eMMC 驱动（Core1 线程）

#### 4.3.1 核心变更

| hp640 SPL | HP232x RT-Thread Core1 |
|-----------|----------------------|
| ARMv7 MMIO + u-boot DM | ARMv8 MMIO + RT-Thread Device Driver |
| `get_timer()` | `rt_tick_get()` |
| `mdelay()` / `udelay()` | `rt_thread_mdelay()` / `rt_udelay()` |
| ADMA3 描述符（DDR 地址） | IRAM1 静态分配 |
| 64 位地址 | 64 位地址（ARMv8） |
| 中断驱动 | **无中断，纯轮询** |

#### 4.3.2 Core1 线程结构

```c
/* eMMC 请求结构体 — Core0 通过静态共享内存发送给 Core1 */
struct spl_emmc_request {
    uint8_t  op;          /* EMMC_OP_READ / WRITE / INIT / SET_MODE */
    uint32_t addr;        /* eMMC 逻辑块地址 */
    void    *buf;         /* 数据缓冲区（IRAM1 内） */
    uint16_t blk_cnt;
    uint16_t blk_size;
    int      result;      /* Core1 回填结果 */
    uint8_t  done;        /* 完成标志 */
    uint8_t  reserved[3]; /* 对齐到 8 字节 */
} __aligned(8);

/* ���态共享内存（不占用 heap） */
static struct spl_emmc_request s_emmc_req __aligned(8);
static uint8_t s_emmc_read_buf[8 * 512] __aligned(8);  /* 8 blocks = 4KB */
static uint8_t s_emmc_write_buf[8 * 512] __aligned(8);

/* Core1 线程入口 */
static void spl_emmc_core1_entry(void *param)
{
    while (1) {
        if (s_emmc_req.done == 0 && s_emmc_req.op != 0) {
            switch (s_emmc_req.op) {
            case EMMC_OP_INIT:
                s_emmc_req.result = emmc_init_driver();
                break;
            case EMMC_OP_READ:
                s_emmc_req.result = emmc_read_data(
                    s_emmc_req.addr, s_emmc_req.buf,
                    s_emmc_req.blk_cnt, s_emmc_req.blk_size);
                break;
            case EMMC_OP_WRITE:
                s_emmc_req.result = emmc_write_data(
                    s_emmc_req.addr, s_emmc_req.buf,
                    s_emmc_req.blk_cnt, s_emmc_req.blk_size);
                break;
            }
            s_emmc_req.done = 1;
        }
        rt_thread_mdelay(1);  /* 让出 CPU */
    }
}
```

#### 4.3.3 内存规划

eMMC 相关静态内存分配（总计约 12KB）：

| 变量 | 大小 | 用途 |
|------|------|------|
| `s_emmc_req` | 32B | 请求结构体 |
| `s_emmc_read_buf` | 4KB | 读数据缓冲（8 blocks） |
| `s_emmc_write_buf` | 4KB | 写数据缓冲（8 blocks） |
| `s_interg_base[]` | 128B | ADMA3 描述符表 |
| `s_pair_base[]` | 32KB | ADMA3 配对描述符（256 entries） |
| **合计** | **~44KB** | |

> ⚠️ **44KB 超出 IRAM1 可用空间**。需要大幅缩减：
> - ADMA 描述符从 256 降至 32
> - 数据缓冲从 8 blocks 降至 2 blocks（2KB × 2 = 4KB）
> - 总计约 **8KB**，可以接受

### 4.4 APU 控制

KA200 有 APU 协处理器，可以直接移植寄存器访问代码：

```c
/* KA200 APU 寄存器地址（从 spl_cmd.c 提取） */
#define REG_APU_CTRL_ADDR       0x12500074
#define REG_APU_BASE            0x1400000000ULL

int apu_write_register(uint64_t addr, uint32_t value);
uint32_t apu_read_register(uint64_t addr);
int apu_load_program(uint64_t src_addr, uint64_t dst_addr, uint32_t len);
```

### 4.5 PVT 模块

KA200 有 PVT IP，可以直接移植。寄存器地址从 `spl_pvt.c` 提取：

```c
#define APB_PVT_BASE_ADDR       0x12400000ULL
/* ... PVT 寄存器偏移 ... */
```

### 4.6 EFUSE 模块

直接移植 `lx_efuse.c`，寄存器地址相同：

```c
#define EFUSE_BASE_ADDR         0x12300000
/* ... EFUSE 寄存器偏移 ... */
```

### 4.7 I2C 接口

KA200 有 DesignWare I2C 控制器，可以直接移植协议层：

```c
/* I2C 基地址（从 hp640 代码推断） */
#define I2C0_BASE               0x10005000ULL  /* 待确认 */
```

### 4.8 Flash（SPI）接口

SPI Flash 通过 AXI 总线访问，地址映射：

```c
/* XIP 地址（从 spl_cmd.c） */
#define FLASH_XIP_BASE          0x90000000UL
```

### 4.9 CLI 命令集

通过 RT-Thread FinSH 注册命令：

```c
#include <finsh.h>

/* 注册 SPL 命令到 FinSH */
static void spl_cmd_register(void)
{
    /* eMMC 命令 */
    shell_register_cmd("emmc_init", cmd_emmc_init, "Initialize eMMC");
    shell_register_cmd("emmc_read", cmd_emmc_read, "Read from eMMC");
    shell_register_cmd("emmc_write", cmd_emmc_write, "Write to eMMC");

    /* Flash 命令 */
    shell_register_cmd("flash_read", cmd_flash_read, "Read from SPI Flash");
    shell_register_cmd("flash_write", cmd_flash_write, "Write to SPI Flash");

    /* 日志命令 */
    shell_register_cmd("log_show", cmd_log_show, "Display log buffer");
    shell_register_cmd("log_level", cmd_log_level, "Set log level");

    /* PVT 命令 */
    shell_register_cmd("pvt_dump", cmd_pvt_dump, "Dump PVT sensors");

    /* APU 命令 */
    shell_register_cmd("apu_ctrl", cmd_apu_ctrl, "Control APU");

    /* EFUSE ��令 */
    shell_register_cmd("efuse_uuid", cmd_efuse_uuid, "Read chip UUID");
}
INIT_ENV_EXPORT(spl_cmd_register);  /* 环境初始化阶段注册 */
```

---

## 5. RTOS 任务设计

### 5.1 任务列表

| 任务名 | 优先级 | 栈大小 | 说明 |
|--------|--------|--------|------|
| **emmc_core1** | 1 (最高) | 2KB | eMMC 主循环线程，无中断 |
| **spl_cmd_task** | 5 | 1KB | 处理 SPL 命令请求 |
| **log_export_task** | 10 | 512B | 定期导出日志缓冲区 |

### 5.2 任务创建（在 main.c 或初始化阶段）

```c
static rt_thread_t s_emmc_thread;

int spl_subsys_init(void)
{
    /* 1. 初始化日志系统 */
    spl_log_buffer_init(&g_spl_log_buffer);
    spl_log_set_buffer(&g_spl_log_buffer);
    spl_log_init();
    SPL_INFO("SPL log system initialized\n");

    /* 2. 初始化 eMMC Core1 线程 */
    s_emmc_thread = rt_thread_create("emmc_core1",
                                      spl_emmc_core1_entry, RT_NULL,
                                      2048, 1, 10);
    if (s_emmc_thread) {
        rt_thread_startup(s_emmc_thread);
    }

    /* 3. 注册 FinSH 命令 */
    spl_cmd_register_all();

    return 0;
}
INIT_ENV_EXPORT(spl_subsys_init);
```

### 5.3 Core0 → Core1 通信

通过 **静态共享内存 + 原子标志**：

```
Core0 线程                      Core1 (emmc_core1) 线程
    │                                │
    │  1. 填充 s_emmc_req             │
    │  2. 设置 op, buf, blk_cnt       │
    │  3. s_emmc_req.done = 0         │
    │  4. 自旋等待 done == 1          │
    │ ◄────────────────────────────── │
    │                                │ 5. 检测到 req.done == 0
    │                                │ 6. 执行 emmc_read/write
    │                                │ 7. 回填 result
    │                                │ 8. s_emmc_req.done = 1
    │  9. 读取 result                 │
    │                                │
```

---

## 6. 内存总规划

IRAM1 256KB 分配：

| 区域 | 地址 | 大小 | 用途 |
|------|------|------|------|
| .bss (现有) | 0x100050000 | ~52KB | 内核 BSS |
| **SPL 日志缓冲区** | 0x100050000 | **32KB** | spl_log_buffer |
| **eMMC 请求结构** | +32KB | **32B** | s_emmc_req |
| **eMMC 读缓冲** | +32KB | **2KB** | s_emmc_read_buf |
| **eMMC 写缓冲** | +34KB | **2KB** | s_emmc_write_buf |
| **eMMC ADMA 表** | +36KB | **4KB** | s_interg_base + s_pair_base |
| Page Pool (现有) | — | 16KB | RT-Thread 页池 |
| Heap (现有) | — | 32KB | Small Mem |
| Stack (现有) | — | ~92KB | 线程栈 |

> **总计新增**：约 **40KB**（日志 32KB + eMMC 8KB）
> **剩余 IRAM1 可用**：256KB - 52KB(bss) - 40KB(spl) - 16KB(page) - 32KB(heap) = **116KB** (足够 stack)

---

## 7. 移植步骤

### Phase 1: 基础设施（1 天）

1. 创建 `spl_log_buffer.c` — 日志缓冲区（IRAM1 静态分配）
2. 创建 `spl_log.c` — 日志输出（rt_kprintf）
3. 创建 `spl_error_code.h` — 错误码枚举

### Phase 2: 片上 IP 模块（1-2 天）

4. 创建 `spl_efuse.c` — EFUSE 模块（直接移植 lx_efuse.c）
5. 创建 `spl_apu.c` — APU 控制（KA200 有 APU）
6. 创建 `spl_pvt.c` — PVT 模块（KA200 有 PVT IP）

### Phase 3: 外设驱动（2 天）

7. 创建 `spl_flash.c` — SPI Flash 封装
8. 创建 `spl_i2c.c` — I2C 从机协议层

### Phase 4: eMMC Core1 线程（2-3 天）

9. 创建 `spl_emmc.h` — eMMC 公共接口
10. 创建 `spl_emmc_core1.c` — eMMC 驱动核心（轮询模式）
11. 创建 `spl_emmc_api.c` — 对外 API + 共享内存管理

### Phase 5: CLI 集成（1 天）

12. 创建 `spl_cmd.c` — FinSH 命令注册
13. 创建 `applications/spl_main.c` — 子系统入口
14. 修改 `applications/SConscript` — 添加 SPL 模块

### Phase 6: 测试验证（2-3 天）

15. 日志系统测试
16. eMMC 读写测试
17. Flash 读写测试
18. EFUSE UUID 读取测试
19. PVT 温度/电压读取测试

---

## 8. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| IRAM1 空间紧张 | eMMC + 日志可能溢出 | 日志缓冲区降至 16KB；eMMC 缓冲降至 2 blocks |
| eMMC ADMA 描述符过大 | 32KB 描述符表占 IRAM1 1/8 | 缩减 ADMA 链到 32 个条目 |
| Cache 一致性 | ARMv8 L1/L2 cache 可能导致数据不一致 | 使用 RT-Thread 的 cache 管理 API |
| SMP 竞争条件 | Core0 和 Core1 并发访问共享内存 | 使用 `__DSB` + `__ISB` 屏障 + atomic 操作 |
| 64 位地址 | KA200 使用 64 位物理地址 | 所有 MMIO 使用 `uint64_t` 地址 |

---

## 9. 文件最终布局

```
bsp/lynxi/hp232x/
├── drivers/
│   ├── spl_log_buffer.c      # 新增：日志缓冲区管理
│   ├── spl_log.c             # 新增：日志输出
│   ├── spl_efuse.c           # 新增：EFUSE 模块
│   ├── spl_apu.c             # 新增：APU 控制
│   ├── spl_pvt.c             # 新增：PVT 模块
│   ├── spl_flash.c           # 新增：SPI Flash
│   ├── spl_i2c.c             # 新增：I2C 从机协议
│   ├── spl_emmc_core1.c      # 新增：eMMC 驱动核心
│   ├── spl_emmc_api.c        # 新增：eMMC API
│   └── spl_cmd.c             # 新增：FinSH 命令注册
│
├── applications/
│   ├── spl_main.c            # 新增：SPL 子系统入口
│   └── SConscript            # 修改：添加 spl_main.c
│
├── Kconfig                   # 修改：添加 SPL 模块配置
└── doc/
    └── SPL_MIGRATION_DESIGN.md  # 新增：本文档
```

---

## 10. 版本信息

| 项目 | 值 |
|------|------|
| 文档版本 | V1.0 |
| 编写日期 | 2026-07-11 |
| 源端版本 | hp640_arm SPL (spl_cmd.c 7172 行) |
| 目标平台 | KA200 ARMv8-A, RT-Thread 5.3.0 SMP |
| 目标 BSP | bsp/lynxi/hp232x/ |
