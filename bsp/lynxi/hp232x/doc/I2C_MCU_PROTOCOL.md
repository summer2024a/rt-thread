# I2C MCU 协议设计与汇总（KA200 ↔ HP2320 MCU）

> 范围：RTT `i2c_mcu` 从机 + `biz_i2c_proxy` 邮箱代理。  
> 对端：`lynxi-mcu` HP2320 APP（I2C2 master）+ Host `mcu-tools`（经 FPGA UART）。  
> MCU 详述：`lynxi-mcu/.../Doc/I2C_PROTOCOL.md`、`MCU_OTA_PHASE_B.md`。  
> 更新日期：2026-07-23

## 1. 角色与拓扑

```text
Host mcu-tools
    │  FPGA UART
    ▼
HP2320 MCU (I2C2 master, PCA9545)
    │  Mem_Write/Read reg=0x00
    ▼
KA200 DW I2C0 slave @ 0x34+(chip&7)
    │  IRQ63 → i2c_mcu BH
    ▼
biz_i2c_proxy_handle()  →  MMIO / IRAM OTA / FlashWrite
```

| 侧 | 固件 | 职责 |
|----|------|------|
| KA200 | 本 BSP RTT | `drv_i2c.c` 从机；`biz_i2c_proxy.c` 解析邮箱 |
| MCU | HP2320 APP | 选 mux、组帧、超时；OTA 中继 524B→I2C 切片 |
| Host | mcu-tools | `-r/-w ka200_reg`；`-u --target ka200` |

线程：`i2c_mcu` @CPU0（优先 3）；FlashWrite bounce 到 `flash` worker @CPU0。

## 1.1 编译 / 运行开关（与 MCU 不对等）

| 项 | MCU | RTT（本 BSP） |
|----|-----|----------------|
| 代理框架 | `HP2320_KA200_I2C_PROXY`（CMake，默认 ON） | `biz_i2c_proxy.c` **始终编入** biz（`SConscript` 未按 MOD 裁剪） |
| OTA `0xE8..0xEB` | `HP2320_KA200_OTA`（须 PROXY，默认 ON） | **无独立宏**；与 proxy 同文件常驻 |
| IP `0xD0/0xD1` | `HP2320_KA200_CMD_IP_REG` | 同上，常驻 |
| I2C 线程自启 | N/A（MCU 为 master） | `BSP_I2C_DEFER`：**关**=boot 自启（默认）；**开**=msh `i2c start` |

关掉 MCU 的 `PROXY`/`OTA` 可省 Flash；RTT 侧若要裁 OTA，目前需改代码/`SConscript`（尚无 `BSP_DRV_MOD_I2C_OTA`）。  
MCU 宏表：`lynxi-mcu/.../Doc/BUILD.md`。RTT 其它宏：[BSP_MACROS.md](BSP_MACROS.md)。

## 2. 邮箱帧格式（Mode B）

| 字段 | 偏移 | 说明 |
|------|------|------|
| `cmd_id` | 0 | 命令字 |
| `len` | 1 | payload 字节数 |
| `crc` | 2 | 对 payload 的 8-bit CRC（与 MCU `calcCRC` 一致） |
| `rsvd` | 3 | 0 |
| `payload` | 4… | ≤124；整帧 ≤128（`BIZ_I2C_BUF_MAX`） |

I2C 传输：`Mem_Write(reg=0x00, frame)` →（可选短延时）→ `Mem_Read(reg=0x00, rsp)`。  
写仅成功类命令可不读回（返回 0）。

## 3. 命令汇总

### 3.1 保留 / 日志

| cmd | 名 | 说明 |
|-----|-----|------|
| `0x01` | READ_LOG | 兼容 hp640 读日志环（验收状态见 BIZ_PORTING） |

### 3.2 Pull 查询（MCU 默认可关）

| cmd | 名 | 响应概念 |
|-----|-----|----------|
| `0xC0`–`0xC3` | TEMP/TIME/VOL/STATUS | 字符串或短状态 |

### 3.3 IP 寄存器代理（主路径）

| cmd | 名 | 请求 payload |
|-----|-----|--------------|
| `0xD0` | READ_IP_REG | `{ u32 reg_addr LE; u8 access_len }`（5B） |
| `0xD1` | WRITE_IP_REG | 同上 + data |

- **绝对 MMIO 地址**（无 base+offset）。
- 读/写优先对齐 32/16-bit 访问；对 CPR 等勿 byte 读。
- `access_len` ≤ `BIZ_I2C_IP_ACCESS_MAX`（32）。

### 3.4 固件 OTA（Phase B）

| cmd | 名 | 请求 | 行为 |
|-----|-----|------|------|
| `0xE8` | OTA_OPEN | `total_size` + `img_crc32` + `flash_addr` | 绑定 `IRAM1_HOST_SCRATCH`；state=`recv`；预建 commit worker |
| `0xE9` | OTA_DATA | `offset` + `chunk[]` | `memcpy(scratch+offset)`；chunk ≤ ≈120 |
| `0xEA` | OTA_COMMIT | 可选 flags | `sem_release` → 异步 CRC（可选）+ `drv_flash_write`；立刻 `writing` |
| `0xEB` | OTA_STATUS | — | `{ state, detail, recv_bytes }` |

**state**：`0 idle / 1 recv / 2 writing / 3 ok / 4 fail`  
**detail（fail）**：`1 BAD_PARAM / 2 OVERFLOW / 3 CRC / 4 FLASH / 5 BUSY / 6 INCOMPLETE / 7 THREAD`

默认 `flash_addr = 0xA6000`。镜像须 ≤ 256KB。  
`program ok` = 页 PP + WIP + 回读通过。冷启消费须 **Head640** 布局 + **冷复位**（见 `IMAGE_DESIGN.md`）。

## 4. OTA 缓冲链（与 MCU 对齐）

```text
Host 文件 → UART 524B → MCU static 524B 快照
         → I2C ~120B 片 → KA200 IRAM1 256KB 拼图
         → COMMIT → SPI Flash @0xA6000
```

- MCU **不**缓存整镜像。
- COMMIT **不得**在 I2C ISR/BH 同步堵死 FlashWrite；由 `i2cota` 线程执行。
- MCU 轮询 `0xEB` 至 `ok` 后再回 Host UART `0x5B`。
- **Cache**：I2C DATA 用 CPU `memcpy` 填 WB scratch；COMMIT 前必须 **dcache clean/flush** 再 `drv_flash_write`（其内部会 `invalidate_dcache_all`）。Host eMMC Load 是 DMA→DRAM + invalidate，无此问题。

## 5. 实现文件

| 文件 | 内容 |
|------|------|
| `biz/biz_i2c_proxy.h/.c` | 邮箱命令、OTA 会话、FlashWrite 触发 |
| `drivers/drv_i2c.c` | 从机 FIFO、STOP 后交 BH 处理完整帧 |
| `drivers/drv_flash.c` | erase/program/页 verify（`src_WB`） |
| `drivers/board.h` | `IRAM1_HOST_SCRATCH_*` |

MCU：`ka200_i2c_cmd.*`、`ka200_ota_relay.c`、`uart_to_soc.c`。  
Host：`mcu_tools`（`--target ka200`，`MCU_OTA_PKT_GAP_MS`）。

## 6. 联调速查

```bash
# IP reg（chip30）
mcu-tools -l 0 -i 2 -t 1 -r ka200_reg -v 30,0x12500064,4

# Phase B OTA（Head640）
mcu-tools -l 0 -i 2 -t 1 -u HP232x_KA200_Serdes_Update_*_Head640.bin \
  --target ka200 --soc 30 -d

# 加大包间 gap（ms，下限 800）
MCU_OTA_PKT_GAP_MS=1200 mcu-tools ... --target ka200 --soc 30
```

KA 串口期望：`I2C OTA COMMIT` → `[flash] erase ok, programming...` → `program ok` → `I2C OTA OK FlashWrite`。  
板级移植状态见 [BIZ_PORTING.md](../BIZ_PORTING.md) §4.5。
