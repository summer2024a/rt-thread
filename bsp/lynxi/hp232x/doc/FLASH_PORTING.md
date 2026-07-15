# HP232x Flash 移植与运维说明

> SPI NOR（Boot SSI `ssi2` @ `0x02000000`）在 RT-Thread 上的操作流程、踩坑与 CPU0/CPU1 配置。  
> 代码：`drivers/drv_flash.c` / `drv_flash.h`；Host 升级：`biz/biz_emmc_exec.c`；对齐参考：hp640 SPL + Linux `lynxi-linux/.../lyn-sfc.c`。  
> 测试环境见 [../Test_ENV.md](../Test_ENV.md)，业务总览见 [../BIZ_PORTING.md](../BIZ_PORTING.md)。  
> 镜像组包与 BL1→BL22 / 串口·Host 路径见 [IMAGE_DESIGN.md](IMAGE_DESIGN.md)。

---

## 0. 最新板测结论（2026-07-14）

测试机 **192.168.58.36**，拓扑 `-l 0 -i 2 -k 30`；一键 `python3 scripts/flash_host_upgrade_test.py`。

| 项 | 结果 | 说明 |
|----|------|------|
| Host Load + FlashWrite `@0xA6000` | ✅ | `OK Load` + `OK FlashWrite` + 回读非全 `FF`；整包 verify 一次过 |
| JEDEC | ✅ | EVB：`0xc22537`（known） |
| 生产：CPU0 flash worker | ✅ | 默认；与 SPL「Flash 归 Core0」一致 |
| 试验：CPU1 直写 SSI | ✅ | `BSP_FLASH_DIRECT_ON_CALLER` PASS；**默认仍关**，回 worker |
| Host scratch **NC**（`BSP_IRAM1_LOW_NC`） | ✅ | 日志 `host scratch NC` + `src_NC`；**当前默认开** |
| Host scratch **WB**（关宏） | ✅ | A/B：`host scratch WB` + `src_WB`；inv-all + 页 bounce |
| eMMC ADMA 描述符区 | 不变 | 仍在 **`IRAM1_DMA_NC` @ `0x100040000`（64KB）**；与 Host scratch 分离 |
| Flash 冷启 SSI/JEDEC（jumper 留下 `ba`） | 🟡 | inv+B8+98 + PA0 alias；**勿 INIT_DEVICE JEDEC**（SEA）；`main` `drv_flash_bringup`；见 BIZ_PORTING **§4.7** |
| 勿改 hp640 jumper | 约束 | `BOOT_SELECT` strap 只读；`entry_point` 不早写 98 |

**稳定性路径回顾（已合入）**

1. **协议**：RDSR/JEDEC 用 **EPROMREAD**（非 TR dummy）；写用 **TMOD_TO** — 修好后假 ready / 全 `0xFF` 消失。  
2. **跨核 cache**：CPU1 Load → `@0x100000000`，CPU0 擦写；先做 **WB + `invalidate_dcache_all` + 按页 bounce/页 verify**（曾 32×手动升级无 mismatch）；再加 **`BSP_IRAM1_LOW_NC`**，A/B 两边均 PASS。  
3. **页级 PP verify**：NC/WB 下都保留（防单页洞）。整包 verify 失败仍最多重试约 10×（兜底）。  
4. **冷启 leave-XIP**：jumper 留 `ssi_ctrl=ba`；须 **dcache inv + B8 + 98**（裸 `mw` 不稳）；SSI 用 **`0x07000000→PA0`**，勿 VA=0、勿改 SEL。**SSI/JEDEC 放 `main`（或 msh），不要放 `INIT_DEVICE`。**

---

## 1. 硬件与地址

| 项 | 值 | 说明 |
|----|-----|------|
| 控制器 | DesignWare AHB SSI（`ssi2`） | DTS 三窗：`0x02000000` / `0` / `0x06000000`，由 `boot_sel` 选窗 |
| 总线时钟假定 | 100 MHz | `FLASH_BUS_CLK_HZ` |
| Probe / 常态波特 | 100 kHz / 24 MHz | 对齐 open_flash 探速 + 稳妥常态 |
| XIP 门控 | `0x12600024` | 关 XIP：`0xB8000000`；SPI/AHB：`0x98000000` |
| Host 升级落点 | `@0xA6000` | `ka200_tools` FlashWrite |
| Host Load 缓冲 | `@0x100000000` | IRAM1 低 256KB；NC/WB 由宏切换（§3.1） |
| eMMC ADMA / DMA 区 | `@0x100040000` 64KB | `.dma_nocache`；**不随 Host scratch 宏改动** |
| 安全调试区 | `@0x900000` | `DRV_FLASH_TEST_ADDR`（msh `flash test`） |
| Bootcode 保护 | `[0, 0x64000)` | 写/擦拒绝（与 hp640 `CONFIG_SPL_640_HEAD` 一致） |

---

## 2. 推荐操作流程

### 2.1 驱动层单笔事务（poll + PIO）

```
XIP disable → SPI/AHB mode
  → SSI disable → drain RX → baud / SER / CTRLR1(NDF) / CTRLR0(TMOD) / TXFLTR / RXFLTR=0
  → SSI enable
  → 按 TMOD 推 DR / 读 DR（勿让 TXFIFO 中途抽空掉 CS）
  → 等 TF_EMPT && !BUSY → SER=0 → SSI disable
```

| 操作 | TMOD | 说明 |
|------|------|------|
| RDSR / JEDEC / 读 reg | **EPROMREAD** | 只发 opcode（+必要时 addr），RX = NDF+1 字节；**无 TR dummy** |
| WREN / SE / BE / PP | **TO（TX only）** | 整帧连续 `flash_tx_only`，对齐 lyn-sfc / 避免 RX 灌爆 |
| 数据读 | **EPROMREAD** | cmd+3B addr 后收 payload |

编程循环：`WREN → 查 WEL → PP/擦 → EPROMREAD RDSR 等 WIP=0`。

### 2.2 Host 在线升级链

```
lynx-showinfo -r -l 0 → xmodem 启 RTT（BL1→BL21→BL22）→ heartbeat
  → lynx-showinfo 拓扑 ALIVE
  → ka200_tools -u -l 0 -i 2 -k 30 -f
       Host → eMMC @0x600000
       emmc_biz@CPU1: Load → IRAM @0x100000000
       FlashWrite → SPI @0xA6000（默认 bounce → CPU0 flash worker）
       整包 readback verify
```

一键：`python3 scripts/flash_host_upgrade_test.py`  
成功判据：`OK Load` + `OK FlashWrite` + `flash read @0xa6000` 非全 `FF`；日志带 `src_NC` 或 `src_WB` 与宏一致。

### 2.3 MSH 调试

```
flash info
flash read  [addr=0x900000] [len]
flash write [addr=0x900000] [len]   # 写样型
flash erase [addr=0x900000] [len]
flash test  [addr=0x900000] [len]   # 擦写校验
flash ssi_probe [0|1]               # 分核直连 SSI（诊断，见 §5）
```

---

## 3. CPU0 worker / CPU1 直写 / Host scratch NC·WB

### 3.1 配置（`rtconfig.h`，当前推荐）

```c
#define BSP_BIZ_EMMC_ON_CPU1          /* emmc_biz @ CPU1，优先级保持 biz 最高(2) */

#define BSP_FLASH_CPU0_WORKER         /* 默认：读/写/擦 bounce → CPU0「flash」线程 */
/* #define BSP_FLASH_DIRECT_ON_CALLER */  /* 试验：CPU1 直写已 PASS；生产保持关 */

#define BSP_IRAM1_LOW_NC              /* Host scratch → Normal NC（默认） */
/* #define BSP_IRAM1_LOW_NC */        /* A/B：关 = WB + cache 维护；板测同样 PASS */
```

| 宏组合 | 启动 / 写 Flash 日志 | 行为 |
|--------|----------------------|------|
| **LOW_NC 开**（默认） | `host scratch NC` · `src_NC` | MMU 映低 256KB NC；Load/Flash **跳过** 对该区的 flush/inv-all；**页 bounce 仍开** |
| LOW_NC 关 | `host scratch WB` · `src_WB` | 区仍是 WB；擦前/擦后 **`invalidate_dcache_all`** + 页 bounce；verify 前 inv src |
| DIRECT 开 | `flash DIRECT ...` · `(cpu1 …)` | SSI 在调用核跑；**试验**用 |

**eMMC 描述符**：`emmc_addr_in_dma_nc()` **只**认 `IRAM1_DMA_NC_*`；Host scratch 跳过 dcache 走单独判断，**勿把描述符挪进 `@0x100000000`**。

改宏后需重新 `scons` 并重新 Xmodem / 升级固件。

### 3.2 优先级与调度注意

| 线程 | 优先级（数字越小越高） | 绑定 |
|------|------------------------|------|
| `emmc_biz` | **2（biz 最高，勿改低）** | CPU1 |
| `flash` worker | 5 | SSI 在 worker 上下文（CPU0） |
| `fl_prb`（ssi_probe） | 3 | 探测目标核 |

**必须遵守：**

1. **不要**把探测/其它线程调到比 `emmc_biz` 更高优先级去「抢」CPU1。  
2. `query_task` 无任务时对 eMMC **忙等且不让出**；CPU1 上更低优先线程可能 TIMEOUT（不一定是 SError）。  
3. 从 CPU1 **`rt_kprintf` + msh 等待** 易与 console 死锁；`ssi_probe` 目标核禁止打印。

### 3.3 为何仍推荐 CPU0 worker +（默认）scratch NC

- 与 SPL 双核分工一致（Core0 Flash；Core1 eMMC）。  
- SSI 经 worker + mutex 串行化。  
- NC 去掉跨核 load/store 的 inv-all 依赖，路径更干净；WB 路径保留作对照。  
- CPU1 直写协议修好后可 PASS，仅作例外开关。

---

## 4. 踩坑与处理

### 4.1 FlashWrite 结束但内容全 `0xFF`

| 原因 | 处理 |
|------|------|
| RDSR 用 **TR**：`rx[0]` 是 dummy，**SR 在 `rx[1]`**；假 ready | **EPROMREAD**，WIP/WEL 看真实 SR 字节（对齐 lyn-sfc） |
| 擦写未等 WIP | `flash_nor_wait_ready()` 循环 EPROMREAD RDSR bit0 |
| PP 中途 TXFIFO 抽空 → CS 抬 → 编程无效 | `flash_tx_only` 连续填 |

### 4.2 间歇 verify `rd=0xff` / 整包多次重擦（已缓）

| 原因 | 处理（已合入） |
|------|----------------|
| CPU1 Load WB、CPU0 读 src：擦长窗口 + stale line | WB：`invalidate_dcache_all` 擦前/擦后；或开 **`BSP_IRAM1_LOW_NC`** |
| 单页未写上仍被整包当数据错 | **按页 memcpy bounce + 页级回读失败重 PP** |
| 整包仍对不齐 | `exec_task_flash_write` 整包 verify 失败整段重试（上限约 10×） |

NC 开启时写路径日志应为一次 `erase ok → program ok`，不再依赖整包重试才成功（A/B 如此）。

### 4.3 SError（SSI MMIO）

| 现象 | 处理思路 |
|------|----------|
| 早期错 TMOD / FIFO 灌满 | 写 **TO**，读 reg **EPROMREAD** |
| 误以为「CPU1 绝不能碰 SSI」 | 协议修好后 `ssi_probe 1` + DIRECT 可 PASS；优先查协议再亲和性 |
| 探测挂起 `phase=0` | biz 忙等占满 CPU1、探测优先过低、目标核 `kprintf` 死锁 |
| **Flash 冷启 win=1 碰 SSI@0 SError** | 对齐 U-Boot/lyn-sfc：`0x98000000` + strap 窗。win1 物理是 **PA0**；Linux `ioremap(0)` 用**高 VA→PA0**，不是 VA=0。RTT identity 下 `regs=0` 会 SEA → MMU 别名 `0x07000000→PA0`（等同 ioremap） |

定位：`flash ssi_probe 0` / `1`（p1 裸 MMIO → … → p5 测试区擦写）。看 `phase` / `SError ESR/FAR`。

### 4.4 其它

| 问题 | 处理 |
|------|------|
| JEDEC 非 MX25（如 `0xc22537`） | `flash_jedec_is_known[]` 已含；未知仅 WARN |
| 写 boot 区失败 | 勿改 `[0,0x64000)`；调试用 `@0x900000` |
| Host 「Successfully」但无 `OK FlashWrite` | 以 UART **`OK FlashWrite`** + 非 FF 为准 |
| Load 日志 `size=` 异常大 | `HP640_Task` union：Load 看 **blks / bytes=blks×512**，勿读 `size` |
| `ka200_tools` Load 字节 > 输入文件 | 工具侧 `repack`（4K+MD5+512 对齐），见 [IMAGE_DESIGN.md](IMAGE_DESIGN.md) |
| XIP 未关就 AHB | 每次 `flash_run_*` 先关 XIP 再 SPI/AHB |
| 日志行被粘连 | `BIZ_LOG_ENTRY_MAX_LEN` 已加大并强制尾 `\n` |

---

## 5. 自检清单

```bash
cd bsp/lynxi/hp232x
python3 scripts/flash_host_upgrade_test.py
```

串口应出现（默认 NC + worker）：

```
[mmu] IRAM1 host scratch NC 0x100000000 +0x40000 (BSP_IRAM1_LOW_NC)
[drv] flash worker on CPU0
[flash] erase+program addr=0xa6000 len=... (cpu0 src_NC)
[flash] program ok
[biz][upgrade] OK FlashWrite ...
```

A/B（关 `BSP_IRAM1_LOW_NC` 后重编重测）应变为 `host scratch WB` + `src_WB`，判据同左。

期望勾选：

- [ ] JEDEC known，`flash test @0x900000` OK  
- [ ] Host：`OK Load` + `OK FlashWrite`，`@0xa6000` 非全 FF  
- [ ] 生产：**CPU0 worker**，**关** `DIRECT`；scratch **NC 默认开**（WB 作对照）  
- [ ] `emmc_biz` 仍为 biz 最高优先；描述符仍在 DMA NC 区  
- [ ] 冷启动从 `@0xA6000` 进 RTT：**未做**（仍 Xmodem）

---

## 6. 关键文件

| 路径 | 作用 |
|------|------|
| `drivers/drv_flash.c` | SSI PIO、EPROMREAD/TO、CPU0 worker、页 bounce、NC 跳过 inv |
| `drivers/drv_flash.h` | 地址/页大小/API |
| `biz/biz_emmc_exec.c` | Host FlashWrite + verify/retry |
| `drivers/drv_emmc_core.c` | ADMA 区判断不变；Host scratch 单独 skip dcache |
| `drivers/hp232x_mmu.c` / `board.h` | Host scratch NC 映射；`hp232x_addr_is_normal_nc()` |
| `biz/biz_subsys.c` | `drv_flash_worker_start()` |
| `biz/biz_finsh_cmds.c` | `flash` / `ssi_probe` |
| `rtconfig.h` | Worker / DIRECT / `BSP_IRAM1_LOW_NC` |
| `scripts/flash_host_upgrade_*.py` | 一键升级与判据 |
| [IMAGE_DESIGN.md](IMAGE_DESIGN.md) | BL1→BL22、组包、Flash 冷启 vs UART |

对齐参考：

- hp640：`common/spl/spl_cmd.c`（`open_flash` / `write_flash`）、`spi_ahb_master_cfg`、`lynxi_640_read`（冷启 `@0xA6000`）  
- Linux：`drivers/mtd/spi-nor/lyn-sfc.c`（poll + PIO，`TMOD_EPROMREAD` / `TMOD_TO`）
