## 测试环境

HP232X 板测主机与自动化脚本说明。脚本源码位于本目录 [`scripts/`](scripts/)。

### 测试主机1

| 项 | 值 |
|----|-----|
| IP | `192.168.58.36` |
| 账号 | `lynxi` / `lx@123`（sudo 同密码） |
| KA200串口 | `/dev/ttyUSB0` @ 115200 |
| MCU串口 | `/dev/ttyUSB1` @ 115200 |
| KA200复位 | `lynx-showinfo -r -l 0` |
| MCU复位 | `/usr/local/lynx/tools/mcu-tools -l 0 -t 1 -i 2 reset_mcu` |
| 拓扑查询（升级前） | `lynx-showinfo` — 须见 `[Link0] ALIVE` + `[2] ALIVE` |

### 测试主机2

| 项 | 值 |
|----|-----|
| IP | `192.168.49.121` |
| 账号 | `lynxi` / `lx@123`（sudo 同密码） |
| KA200串口 | `/dev/ttyUSB0` @ 115200 |
| MCU串口 | 无 |
| KA200复位 | `lynx-showinfo -r -l 0` |
| MCU复位 | `/usr/local/lynx/tools/mcu-tools -l 0 -t 1 -i 0 reset_mcu` |
| 拓扑查询（升级前） | `lynx-showinfo` — 须见 `[Link0] ALIVE` + `[0] ALIVE` |
| 测试对象 | Link0 Board0 Chip24 |
| 说明 | 串口无 MCU tty；KA I2C addr 常为 `0x34`（mux=0）。Flash `@0xe7000` 若残留 eMMC err（如 `0x6e600020`）会导致旧固件在 HS400 前打 GPIO78 卡住；新固件会先清标志再 init。 |
| A/B 约束 | **同一 Link 同时只允许一个 Host 压测**（fpfifo/dfifo 等互斥）。**Link 或 KA200 离线 → 必须 `lynx-showinfo -r -l 0` 复位**；UART 片再 xmodem（RTT 业务后常需双次）。自动化：`scripts/ab_fpfifo_host2.py --remote`。 |

### 固件与升级方式
#### uart启动方式测试

**编译（开发机）**

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
scons -j$(nproc)
# 产物: rtthread-header.bin
```

**测试机 xmodem 目录**（已搭建，一般无需改动）

```
/home/lynxi/xia/xmodem/
├── boot-wrapper.bin          # BL 跳转包装
├── u-boot-spl.bin  -> ...    # 软链指向 rtthread-header.bin
└── scripts/
    └── hp232x/               # remote_board_test.py 同步脚本到此
```

软链示例（在测试机执行）：

```bash
cd /home/lynxi/xia/xmodem
ln -sf /mnt/49.20/lynxlink/lynxi-rtt/bsp/lynxi/hp232x/rtthread-header.bin u-boot-spl.bin
```

共享挂载路径 `/mnt/49.20/lynxlink/...` 与开发机 `/work/lynxlink/...` 对应；若未挂载，用 `remote_board_test.py upload` 上传固件。

**hp640 SPL（A/B 对比）**

```bash
cd /work/lynxlink/hp640_arm && ./build.sh hp640
# 测试机软链:
cd /home/lynxi/xia/xmodem
ln -sf /mnt/49.20/lynxlink/output/uboot/spl/u-boot-spl-hp640-header.bin u-boot-spl.bin
```

#### flash冷启方式测试
```
1、固件在/home/lynxi/xia/ka200目录
HP232x_KA200_Serdes_Update_20260715_v5.0.bin -> /mnt/49.20/lynxlink/lynxi-rtt/bsp/lynxi/hp232x/HP232x_KA200_Serdes_Update_20260715_v5.0.bin

2、升级命令为
/usr/local/lynx/tools/ka200_tools -u /home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_20260715_v5.0.bin -l 0 -i 2 -k 30

3、复位后固件自动从flash中读取并运行，全程只需要监控串口与交互即可
```

#### MCU固件升级方式
```
/usr/local/lynx/tools/mcu-tools -l 0 -i 2 -t 1 -u HP232x_MCU_serdes_upgrade_20260711_V1.7.5.bin
升级完MCU复位即可生效

---

## 串口信号

| 现象 | 含义 |
|------|------|
| **`.U`** | KA200 已复位，SPL 等待 xmodem，**可以烧录** |
| 烧录完成 | SPL 自动加载 RT-Thread，**不要再 reset**（否则会回到 `.U` 丢失启动 log） |

> 仅看到 `.U` 时板子尚未运行 RT-Thread，不要在此阶段判断 eMMC / SMP 等业务逻辑。

---

## 测试脚本

脚本位于 [`scripts/`](scripts/)，依赖 `pyserial` 与同目录 `xmodem.py`。

| 脚本 | 用途 | 典型判据 |
|------|------|----------|
| [`flash_run_smp.py`](scripts/flash_run_smp.py) | SMP / Core1 | `[PMON][CPU1] PASS`，tick +50/500ms |
| [`flash_run_biz0.py`](scripts/flash_run_biz0.py) | eMMC 业务 | board init、tuning、heartbeat |
| [`flash_host_upgrade_run.py`](scripts/flash_host_upgrade_run.py) | 单 UART：xmodem→拓扑→升级 | heartbeat + topology + ka200_tools |
| [`flash_host_upgrade_test.py`](scripts/flash_host_upgrade_test.py) | 开发机 SSH 一键 Host 升级 | 同上 + `[biz]` |
| [`flash_and_log.py`](scripts/flash_and_log.py) | 通用烧录 + 120s log | 手动 grep |
| [`test_uart.py`](scripts/test_uart.py) | 仅 xmodem 烧录 | 不抓 log |
| [`remote_board_test.py`](scripts/remote_board_test.py) | 开发机 SSH 上传/板测 | upload / smp / biz |
| [`mk_ka200_image.py`](scripts/mk_ka200_image.py) | 打包 host 升级固件 | ≤256KB |

环境变量（可选）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `HP232X_SERIAL` | `/dev/ttyUSB0` | 串口设备 |
| `HP232X_XMODEM_DIR` | `/home/lynxi/xia/xmodem` | boot-wrapper / spl 所在目录 |
| `HP232X_LYNXLINK` / `BOARD` / `CHIP` | `0` / `2` / `30` | ka200_tools 参数 |
| `HP232X_RESET_CMD` | `lynx-showinfo -r -l 0` | xmodem / 升级前复位 |
| `HP232X_SHOWINFO` | `lynx-showinfo` | 升级前拓扑查询（无 `-r`） |
| `HP232X_UPGRADE_RESET_CMD` | 同 `RESET_CMD` | 兼容旧环境变量 |
| `HP232X_LOG` | `/tmp/hp232x_*_<ts>.log` | log 输出路径 |

---

## 用法

### 方式 A：开发机远程一键（推荐）

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
scons -j$(nproc)

# SMP / Core1 板测
python3 scripts/remote_board_test.py smp

# eMMC 业务板测（需关闭 BSP_BIZ_SKIP_THREADS）
python3 scripts/remote_board_test.py biz

# Host 在线升级 + flash 板测
python3 scripts/flash_host_upgrade_test.py

# 仅上传固件
python3 scripts/remote_board_test.py upload
```

脚本会：上传 `rtthread-header.bin` → patch `boot-wrapper.bin` spl 地址 → 同步 `scripts/` 到测试机 → reset → xmodem 烧录 → 抓 log 并打印 PASS/FAIL。

### 方式 B：测试机本地执行

**1）同步脚本到测试机**（首次或脚本更新后）

```bash
# 开发机
scp -r scripts/ lynxi@192.168.58.36:/home/lynxi/xia/xmodem/scripts/hp232x/
```

**2）在测试机上跑 SMP 测试**

```bash
ssh lynxi@192.168.58.36
cd /home/lynxi/xia/xmodem
ln -sf /mnt/49.20/lynxlink/lynxi-rtt/bsp/lynxi/hp232x/rtthread-header.bin u-boot-spl.bin

cd scripts/hp232x
sudo python3 flash_run_smp.py
```

> **必须在 `scripts/hp232x/`（或含 `xmodem.py` 的目录）下执行**，不可复制到 `/tmp/` 运行（会找不到 `xmodem` 模块）。

**3）查看 log**

```bash
grep -aE 'SMP|PMON|gtimer|msh|PASS|FAIL|heart|tuning' /tmp/hp232x_smp_*.log | tail -30
```

### 方式 C：手动分步（调试）

```bash
# 复位（脚本默认 lynx-showinfo -r；也可 mcu-tools reset_mcu）
lynx-showinfo -r -l 0
sleep 5

stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo
# 等到 .U 后:
cd /home/lynxi/xia/xmodem/scripts/hp232x && sudo python3 test_uart.py
timeout 120 cat /dev/ttyUSB0 | tee /tmp/hp232x_manual.log
```

---

## SMP 判据

| 阶段 | 成功 log |
|------|----------|
| Arch timer | `CNTFRQ=31250000`，`rt_hw_gtimer_init ok` |
| CPU1 boot | `[SMP] CPU1 ready` |
| PMON bind | `[PMON][CPU1] PASS: ran 20 samples on CPU1` |
| tick 频率 | `[PMON][CPU1] #N tick=... (+50)` 每 500ms |

可选：`rtconfig.h` 开启 `BSP_USING_HP232X_SMP_BIND_TEST` 后应看到 `[SMP] bind test PASS`。

详见 [doc/SMP_SETUP.md](doc/SMP_SETUP.md)。

---

## 启动链诊断

| 串口 / 拓扑现象 | 含义 | 处理 |
|----------------|------|------|
| 仅 `P2I0` / `IBKSUE` / `BOOT`，log <8KB | RT-Thread **未进 board init** | 查 **IRAM1 BSS 溢出**（见下） |
| `[board] IRAM1 layout` 后无 heartbeat | board/heap/组件 init 问题 | grep `fail`/`Exception`/`trap` |
| `lynx-showinfo` 中 `KA200>30:0.0` | chip30 RTT 未运行 | 先 `flash_run_biz0.py` 确认 heartbeat |
| heartbeat OK 但拓扑无 ALIVE | Host 链路未就绪 | 不要跑 `ka200_tools` |

**BSS 溢出检查（编译后）**：

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
scons -j$(nproc)
grep __bss_end rtthread.map
# 期望: __bss_end < 0x100080000（IRAM1 上限）
grep biz_exec_stress rtthread.map | grep '\.bss'
# 期望: .bss 约 0x10000（64KB×2），勿出现 0x200000（2MB）
```

> 2026-07-14：`EMMC_MAX_BLK_PER_XFER` 增大后，`biz_exec_stress.c` 曾误绑 `EMMC_MAX_TRANSFER` 导致 2MB 静态 BSS、固件停在 `BOOT`。已改为 `STRESS_MAX_BLK=64`。详见 [BIZ_PORTING.md](BIZ_PORTING.md) §8。

---

## eMMC 业务判据

关闭 `BSP_BIZ_SKIP_THREADS` 后，在测试机执行：

```bash
cd /home/lynxi/xia/xmodem/scripts/hp232x
sudo python3 flash_run_biz0.py
```

成功 log：

```
[biz] emmc_biz bound to CPU1
[drv] emmc_biz entry CPU1
[drv] emmc: tuning OK tap=0x34 iter=63
Heart-beat reported successfully (index=1)
Entering main task processing loop
```

脚本会自动检测「仅 BOOT、无 board init」并提示 BSS 问题。

详见 [doc/EMMC_TUNING.md](doc/EMMC_TUNING.md)、[BIZ_PORTING.md](BIZ_PORTING.md)。

---

## 测试任务（对比 hp640）

### hp640 vs hp232x RT-Thread

| 项 | hp640 SPL | hp232x RT-Thread |
|----|-----------|------------------|
| 编译 | `cd hp640_arm && ./build.sh hp640` | `cd lynxi-rtt/bsp/lynxi/hp232x && scons` |
| xmodem 烧录 | `u-boot-spl-hp640-header.bin` | `rtthread-header.bin`（mkimage，含 padding） |
| Host 升级包 | `HP232x_KA200_Serdes_Update_*_v4.11.bin`（addheader） | `HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin`（`mk_ka200_image` / scons） |
| 组包说明 | — | 见 [doc/IMAGE_DESIGN.md](doc/IMAGE_DESIGN.md)（BL1→BL22、mkimage vs mk_ka200） |
| Host 工具 | `ka200_tools -u` → eMMC Load + FlashWrite @ **0xA6000** | 同左，**无需额外 msh 写 flash** |
| 复位 | `lynx-showinfo -r -l 0` | 同左 |
| 升级前拓扑 | `lynx-showinfo` | `flash_host_upgrade_run.py` 自动检查 |
| 业务线程 | SPL mainloop | **emmc_biz @ CPU1** |
| SPI Flash 自测（可选） | SPL 内建 | msh `flash read` @ 0x900000（与 host 升级无关） |

### biz 任务查询接口（hp640 对齐）

| 项 | hp640 SPL (`spl_cmd.c`) | hp232x (`biz_emmc_biz.c`) | 状态 |
|----|-------------------------|---------------------------|------|
| 查询地址 | `HP640_QUERY_TASK_ADDR` **0x0500000** | 同左 `biz_host_proto.h` | ✅ |
| 读块数 / 块大小 | 1 × 512B | 1 × `BIZ_BLK_SIZE`(512) | ✅ |
| 退出条件 | `cmd!=0` 或 `flag&NOT_END` | 同左 | ✅ |
| 轮询心跳 | `process_heartbeat()` | `biz_emmc_process_heartbeat()` | ✅ |
| 心跳上报地址 | `0x400000 \| size` | `biz_config.heart_beat_report_base_address` **0x400000** | ✅ |
| 任务结果上报 | `0x400208` | `task_result_report_base_address` **0x400208** | ✅ |
| DMA cache | `invalidate_dcache_range` 读完成后 | `emmc_cache_invalidate` in `drv_emmc_core.c` | ✅ |
| 任务包长度 | `HP640_TAKS_PKG_LENGTH`=16 | 同左 | ✅ |

> hp640 注释：`0x0500000` 为 FPGA eMMC 模型临时地址（`lynxi_hp640.h` TODO 正式地址 `0x500000`），hp232x 与之保持一致。

### 任务 1：Host 在线升级 KA200 固件到 Flash

`ka200_tools -u` 一次完成：**Host(FPGA) → eMMC → KA200 emmc_biz** 任务链：

1. **Load** — 从 eMMC FIFO 读固件到 IRAM（`HP640_CMD_Load`）
2. **FlashWrite** — 从 IRAM 写入 SPI Flash @ **0xA6000**（`HP640_CMD_FlashWrite`）
3. **Idle** × N — 结束

**58.36 拓扑**（实测参数）：

| 参数 | 值 | 说明 |
|------|-----|------|
| `-l` | **0** | Lynxlink ID |
| `-i` | **2** | Board ID |
| `-k` | **30** | KA200 chip（58.36 实测） |

**一键测试（开发机）**：

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
scons -j$(nproc)
python3 scripts/flash_host_upgrade_test.py
```

流程：

1. **`lynx-showinfo -r -l 0`** 复位 → xmodem 烧录 → 等待 KA200 **heartbeat**
2. **`lynx-showinfo`** 查询拓扑 — 须打印 `[Link0] ALIVE` 且 Board `[2] ALIVE`
3. **`ka200_tools -u`** — eMMC Load + FlashWrite

拓扑正常示例（58.36 实测）：

```
● [Link0] ALIVE:04||I:512B||...
├──● [2] ALIVE:3FFFFFFF||...|KA200>0~29:4.11 30:0.0
```

> 仅 heartbeat 成功但拓扑未 ALIVE 时，**不要执行 ka200_tools**。

**升级失败时 UART 调试**（`biz_emmc_exec.c` / `biz_emmc_biz.c`）：

```
[biz] host task pkg: Load tag=N blks=... -> 0x...
[biz] exec idx=0 cmd=Load ...
[biz] OK Load emmc=0x600000 dst=0x100000000 blks=435 tag=0
[biz] exec idx=1 cmd=FlashWrite flash=0xa6000 size=...
[biz] OK FlashWrite flash=0xa6000 size=222864 tag=1
```

失败示例：

```
[biz] FAIL step=flash_write tag=1 flash=0xa6000 ret=...
[biz] FAIL step=verify tag=1 off=1234
[biz] FAIL step=crc tag=0 calc=... expect=...
```

**手动步骤（测试机）**：

```bash
# 1. Host 升级包（scons 已生成；或手动）
#    → HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin
python3 scripts/mk_ka200_image.py rtthread.bin
cp HP232x_KA200_Serdes_Update_*.bin /home/lynxi/xia/ka200/

# 2. 一键：复位 → xmodem → heartbeat → lynx-showinfo 拓扑 → ka200_tools
cd /home/lynxi/xia/xmodem/scripts/hp232x
sudo python3 flash_host_upgrade_run.py \
  --ka200 /home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin -l 0 -i 2 -k 30

# 2b. 分步（KA200 已在运行）
lynx-showinfo                 # 确认 Link0 / Board2 ALIVE
/usr/local/lynx/tools/ka200_tools -u /home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin -l 0 -i 2 -k 30 -f
```

成功判据（Host + KA200 UART）：

```
[upgrade] heartbeat OK
[upgrade] topology OK (Link0 Board2)
[INFO] Send Update Command/Firmware Successfully
[biz] OK Load ...
[biz] OK FlashWrite flash=0xa6000 ...
```

### 任务 2：UART 确认 RT-Thread 运行

**RT-Thread 正常**（xmodem 启动后，执行 host 升级前须有 heartbeat）：

```
[biz] emmc_biz bound to CPU1
[drv] emmc_biz entry CPU1
[drv] emmc: tuning OK tap=0x34 iter=63
Heart-beat reported successfully (index=1)
Entering main task processing loop
msh >
```

**SPI Flash 自测（可选，与 host 升级无关）**：开发阶段可用 msh `flash read 0x900000` 验证驱动，**不是** host 升级流程的一部分。

### 任务 3：I2C 联调 / MCU `i2c_scan` 卡死（Flash 冷启）— 已修复（2026-07-15）

**前置**：Flash 冷启，`BSP_I2C_DEFER` **关**（boot 自启 I2C），RTT 已出 `I2C MCU slave ready` + HB。

**复现（修复前）**：

```text
KA200 msh > i2c start          # slave ready @ 0x3a
MCU UART2：+++ → i2c_scan      # found 31/32 假 ACK
→ KA200 msh 无回显（CPU0 死）
```

**根因**：

1. MCU `HAL_I2C_IsDeviceReady` 狂扫 → 总线假 ACK（`found 31/32`）。  
2. KA200 DW I2C **IRQ 未在 ISR 里关 mask**：底半部清 raw 前 GIC 反复进 ISR，饿死 CPU0（msh）。  
3. SDA stuck 路径曾 **无限 busy-wait**，可硬锁 CPU0。

**修复**：

| 侧 | 改动 | 状态 |
|----|------|------|
| KA `drv_i2c.c` | ISR 内 `ic_intr_mask=0`，BH 排空后再开；TX_ABRT/SDA stuck 限次 + CLR | ✅ Flash 已升；扛住 scan |
| MCU `cmd_ka200_i2c.c` / `mcu_to_ka200.c` | 假 ACK 洪泛 abort + `lynxi_mcu_i2c_reinit`；probe 间隔 1ms | ✅ 已刷入（先 `lynx-showinfo -r` 再 `-u`） |

**MCU 升级注意**：若 `mcu-tools -u` 报 `extern lock` / 卡住，**先复位 KA200** 再升：

```bash
lynx-showinfo -r -l 0
sleep 5
/usr/local/lynx/tools/mcu-tools -l 0 -i 2 -t 1 -u HP232x_MCU_serdes_upgrade_YYYYMMDD_V1.7.5.bin
/usr/local/lynx/tools/mcu-tools -l 0 -t 1 -i 2 reset_mcu   # 生效
```

**两端联调验收（2026-07-15 @ 58.36；2026-07-16 起 I2C boot 自启）**：

```text
KA  boot          → I2C MCU slave ready @ 0x3a（无需 i2c start）
MCU i2c_scan      → bus fault: ch ACK=8 … found 8/32 (aborted)
KA  msh           → 仍活（不再卡死）
```

两端均要配合：KA 防 IRQ 风暴；MCU 发现假 ACK 提前停扫、reinit 总线。

升级包：`HP232x_KA200_Serdes_Update_20260715_v5.0.bin`；MCU：`Lynchip_mcu_HP2320/build_app/HP232x_MCU_serdes_upgrade_20260715_V1.7.5.bin`。

---

### 板测结果（2026-07-14 @ 58.36，`-l 0 -i 2 -k 30`）

| 步骤 | 结果 | 备注 |
|------|------|------|
| hp640 A/B heartbeat | **PASS** | 同板基准 |
| eMMC tuning (tap 0x34) | **PASS** | 曾稳定验证 |
| emmc_biz @ CPU1 + heartbeat | **PASS** | 2026-07-13；BSS 修复后需复测 |
| Host 拓扑门控脚本 | **PASS** | `lynx-showinfo` Link0 + Board2 |
| ka200_tools host 下发 | **PASS** | `Send Update Command/Firmware Successfully` |
| KA200 Load 448 blk | **FIX** | `EMMC_MAX_BLK_PER_XFER=2048`（原 ret=10） |
| KA200 FlashWrite @ 0xA6000 | **待复测** | 依赖 Load + 固件稳定 boot |
| BSS 溢出致 boot 挂死 | **FIX** | stress 缓冲改 64 blk；`__bss_end` 回 IRAM1 内 |
| Host 升级后 cold reset | **未做** | 复位后仍 `.U`（Flash 冷启动链） |

固件目录：`/home/lynxi/xia/ka200/`（`HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin`）。

**交接后第一步**：

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
scons -j$(nproc)
python3 scripts/flash_host_upgrade_test.py
```

---

## 常见问题

| 现象 | 原因 | 处理 |
|------|------|------|
| log 停在 `BOOT`，无 `[board]` | IRAM1 BSS 溢出 | `grep __bss_end rtthread.map`；见 BIZ_PORTING §8 |
| log 只有 `.U` | 烧录后再次 reset | 烧录后**不要** reset，用 `flash_run_*.py` |
| heartbeat OK 但升级 abort | 拓扑未 ALIVE | 手动 `lynx-showinfo`，确认 `[Link0]` + `[2]` |
| `[biz] FAIL ret=10` | Load blk 超限 | `EMMC_MAX_BLK_PER_XFER` 应为 2048 |
| `No module named xmodem` | 不在 scripts 目录运行 | `cd scripts/hp232x && sudo python3 ...` |
| `rtthread-header.bin` 0 字节 | scp 覆盖失败 | 开发机重新 `scons`，再 upload |
| ka200_tools xlink failed | 参数与本板不符 | **`-l 0 -i 2 -k 30`** |
| NFS 覆盖固件为 0 字节 | 共享挂载 scp 冲突 | 上传到 `/home/lynxi/xia/xmodem/rtthread-header.bin` |
| `i2c start` 后 MCU `i2c_scan` → KA msh 死 | 旧：DW ISR 未 mask | **双端已修**；见任务 3 |
| MCU `+++` 无反应 | 未等 `UART2 CLI ready` / 口不对 | 先 `reset_mcu`，再 guard 后发 `+++` |
| `mcu-tools` lock / 卡住 | Host 锁或链路忙 | **先 `lynx-showinfo -r -l 0`**，再 `-u` |

---

## 相关文档

| 文档 | 内容 |
|------|------|
| [doc/SMP_SETUP.md](doc/SMP_SETUP.md) | SMP 启动 / 冷启定位 / 跨核 / IRQ |
| [doc/EMMC_TUNING.md](doc/EMMC_TUNING.md) | eMMC tuning |
| [BIZ_PORTING.md](BIZ_PORTING.md) | 业务移植与板测状态 |
| [TEST_METHODOLOGY.md](TEST_METHODOLOGY.md) | 49.81 PCIe boot 测试（历史） |
