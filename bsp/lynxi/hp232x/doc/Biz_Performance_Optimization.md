# Biz 性能优化：fpfifo FPS 与 Host IRAM cache

> 范围：`hp232x` 业务热路径（eMMC ExecBD / CRC32）相对 hp640 SPL 的 FPS 差距。  
> 板测：Host2 `192.168.49.121` Link0 Board0 **Chip24**；工具必须用 staging  
> `/mnt/49.20/lynxlink/staging/fifo_test/fpfifo_stress`（勿用旧路径假阴性）。  
> 日期：2026-07-20。

---

## 1. 现象

同参全路径压测：

```bash
./fpfifo_stress -d 0 -B 0:24 -b16 -T 50 -g 0
```

| 侧 | 典型 Avg FPS | 备注 |
|----|--------------|------|
| hp640 | ~4400–4900 | UART 通常 1× |
| RTT（旧默认 NC） | ~3200–3400 | 约慢 **~30%**；RTT biz 后常需 UART **2×** |

包形态（默认带 compare）：

```text
ExecBD → CRC32(size=8192 @ DDR_IRAM_ADDR) → ExecBD
```

`-n` / `--no_compare`：去掉 CRC，仅 ExecBD。

---

## 2. 拆分实验（gap_probe）

脚本：`scripts/gap_probe_host2.py`（先复位，再 UART boot；RTT 2× / hp640 1×）。

同参 `-b16 -T20 -g0`（RTT 曾开 `BSP_BIZ_PHASE_STATS`，约再 -4% FPS）：

| 用例 | FPS | 周期 |
|------|-----|------|
| RTT CRC（IRAM1 **NC** + PHASE） | 3277 | 305 µs |
| RTT `-n`（NC） | 7262 | 138 µs |
| hp640 CRC | 4476 | 223 µs |
| hp640 `-n` | 7039 | 142 µs |

`msh phase`（NC 镜像）：

```text
[phase] avg_query=188us avg_qt_rounds=20 avg_round=613us
[phase] ExecBD  avg=36us
[phase] CRC32   avg=194us   ← 主耗时
```

结论：

1. **`-n` 两边几乎持平**（RTT 甚至略快）→ ExecBD / query / SMP **不是**主因。  
2. **带 CRC 时 RTT 慢 ~27%** → 差距几乎全在 CRC 路径。  
3. 算法已对齐 U-Boot `lib/crc32.c`（固化表 + LE 字批）；慢在 **内存属性**，不是表/循环写错。

---

## 3. 根因：CRC 扫在 NC 窗上

| 项 | 值 |
|----|----|
| CRC 源 | `DDR_IRAM_ADDR` = **`0x100000000`**（`fpfifo_stress` `INPUT_DATA_ADDR`） |
| 长度 | `-b16` → 16×512 = **8192 B** |
| 旧 RTT 映射 | `BSP_IRAM1_LOW_NC` → 该 256KB **Normal NC** |
| hp640 | 同址 **WB** + `invalidate_dcache_range` 再 `crc32` |

NC 下 CPU **每次**字读都不进 cache；顺序扫 8KB ≈ 2048 次全价 IRAM/总线访问 → `CRC32 avg≈194µs`。

WB 下按 **cache line（通常 64B）** 填 L1：

```text
8KB ÷ 64B/行 ≈ 128 次 miss（贵）
同一行内后续字 → L1 hit（几拍，便宜）
```

粗算有效吞吐：NC ~42 MB/s vs WB ~130+ MB/s → CRC 多出约 **100µs/帧** → FPS 从 ~4900 掉到 ~3300。

### 3.1 为何 `-n` 看不出 NC/WB

`-n` 热路径主要是 **DMA ExecBD**。DMA 走硬件通路，不太吃 CPU 对该 VA 的 cache 属性；故 NC/WB 下 `-n` FPS 都在 ~7000 量级。

### 3.2 NC 更适合什么 / 不适合什么

| 更适合 **NC** | 不适合 **NC** |
|---------------|---------------|
| DMA 描述符 / bounce（写少、硬件读） | CPU 热路径大块顺序读（CRC、软件拷） |
| 跨核 mailbox、偶发状态字 | 每帧扫 KB～MB 的工作缓冲 |
| Host Flash 升级 scratch（少踩漏 flush/inv） | 与 hp640 比拼 fpfifo CRC FPS |

口诀：**硬件/偶发共享 → NC；CPU 猛啃 → WB + 在 DMA 边界做 cache 维护。**

同一块 Host scratch 既当 DMA 目标又被 CRC 当工作集时，用 NC 会直接吃掉全路径 FPS。

---

## 4. 验证（IRAM1 低 256KB → WB）

配置：关 `BSP_IRAM1_LOW_NC`（保留 `BSP_IRAM0_LOW_NC`）；关 `BSP_BIZ_PHASE_STATS`。

Boot 可见：

```text
[mmu] IRAM1 host scratch WB 0x100000000 +0x40000 (upgrade uses cache maintain)
```

| 用例 | FPS |
|------|-----|
| RTT CRC（**WB**） | **4911** |
| RTT `-n`（WB） | 6977 |

与 hp640 ~4900 同量级，**CRC 路径差距基本抹平**。CRC 路径仍走 `biz_exec_crc32` 的 `invalidate` + 算完 `flush` ret（因未再派生 `BSP_BIZ_SKIP_HOST_DCACHE`）。

---

## 5. 默认策略（2026-07-20 起）

| 窗口 | 默认 | 宏 |
|------|------|-----|
| IRAM1 低 256KB `@0x100000000`（Host scratch / CRC 缓冲） | **WB** | **`BSP_IRAM1_LOW_NC` 关** |
| IRAM0 低 256KB `@0x04000000`（Host 描述符等，BL22） | NC | `BSP_IRAM0_LOW_NC` 开 |
| IRAM1 DMA arena `@0x100040000` 64KB | NC | 独立 PTE，与 Host scratch 无关 |

`BSP_BIZ_SKIP_HOST_DCACHE` 仅在 **IRAM0+IRAM1 双 NC** 时由 `board.h` 派生；当前默认不再派生 → CRC/Store/report **编译进** dcache 维护（对齐 hp640）。

### 5.1 若需临时开回 IRAM1 NC

场景：强调 Host Flash 升级路径“少写 cache 代码 / 更省心的相干”。

```c
#define BSP_IRAM1_LOW_NC   /* 开回 NC：Flash 升级更省心，fpfifo CRC FPS 回落 ~30% */
```

Flash 在 **WB** 下仍可用既有 invalidate + page bounce（见 `FLASH_PORTING.md`，曾 32× PASS）。

### 5.2 其它已排除项

| 项 | 结论 |
|----|------|
| CRC 表动态生成 / bit 循环 | 已修；非本轮主因 |
| `BSP_BIZ_HOTPATH_NO_TICK_IPI` | 增益 &lt;1%，非主因 |
| query eMMC / ADMA | `-n` 持平已排除 |
| `BSP_BIZ_PHASE_STATS` | 仅测相用；开约 -4% FPS，生产默认关 |
| `BSP_BIZ_LOG_TIMESTAMP` | A/B 须关 |

---

## 6. 复测清单

1. `rtconfig.h`：确认 `BSP_IRAM1_LOW_NC` **注释掉**；`BSP_BIZ_PHASE_STATS` 关。  
2. `scons` + `mkimage.py ... --dest-addr 0x4020000 --next-offset 0x20020`。  
3. Host2：`python3 scripts/ab_fpfifo_host2.py --remote --fpfifo /mnt/49.20/lynxlink/staging/fifo_test/fpfifo_stress --test-time 50 --blk 16`。  
4. 期望：RTT Avg FPS 与 hp640 同量级（~4500–5000，视 T/板温）。  
5. 可选：`gap_probe_host2.py`；开 `PHASE_STATS` 看 `CRC32 avg` 应从 ~194µs 降到与 hp640 周期差一致量级，测完关回。

相关脚本：`scripts/ab_fpfifo_host2.py`、`scripts/gap_probe_host2.py`、`scripts/flash_common.py`。  
宏表：`doc/BSP_MACROS.md` §6。
