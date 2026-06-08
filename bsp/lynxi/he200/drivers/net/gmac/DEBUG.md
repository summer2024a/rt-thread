# HE200 GMAC 调试记录

本文档用于记录 `bsp/lynxi/he200/drivers/net/gmac` 当前网络链路问题的调试状态与操作步骤。

**初始化口径（2026-06）**：对齐 Zephyr `he200_ep` + lynxi-linux——`lynxi_sysctl_lite_gmac_probe_clocks()`（probe 不写 `0x66f`）、RTL8211F 用 **BMCR 软复位**（跳过 `portd:23` GPIO 硬复位，实板 DDR 会挂死）、**`rgmii-id` PHY TX/RX delay**（`lynxi_rtl8211f_rgmii_id_config()`，对齐 Linux `realtek.c`）。详见 [`../../移植记录.md`](../../移植记录.md) §8。

**测试环境**：[`../../scripts/he200_test_env.sh`](../../scripts/he200_test_env.sh) + [`../../测试环境.md`](../../测试环境.md)。直连网段：`Host 192.168.1.1` ↔ `EP 192.168.1.2`（`enp25s0f1`）。

**配置互斥**：调试板载 GMAC 时 **关闭 `BSP_USING_RPMSG_NET`**（Kconfig 已与 GMAC 互斥）。`e0_isr` 须 `rt_hw_interrupt_set_triger_mode(78, 1)`（Linux `IRQ_TYPE_LEVEL_HIGH`）并绑定 CPU0。

## 1. 现象与根因（2026-06-06 更新）

### 1.1 链路层（已解决）

历史现象：

- Host 侧网口（`enp25s0f1`）`Link detected: no`
- 设备侧 `e0` 显示 `LINK_UP`，但与 Host 互 ping 不通
- `ifconfig` 统计中 Host RX 为 0

根因之一：Linux EVB 使用 `phy-mode=rgmii-id`，旧驱动未配置 RTL8211F 内部 delay，导致物理层协商失败（`anlpar=0000`）。已合入 `lynxi_rtl8211f_rgmii_id_config()`。

### 1.2 数据通路（2026-06-06 ping 打通）

链路 up 后仍 ping 不通时的分层结论：

| 层次 | 现象 | 根因 | 修复 |
|------|------|------|------|
| RX 长度 | 收包异常 | DWMAC4 `RDES3[14:0]` 已是帧长，旧代码再减 4 | `lynxi_dwmac4_rx_frame_length()` 直接用 |
| RX pbuf | ICMP 回复失败 | `PBUF_RAW` 头空间不足 | `PBUF_TRANSPORT` + `PBUF_RAM` |
| TX 回收 | 发几包后挂死 | `plat_free_memory` 误释 nocache TX 缓冲 | `lynxi_dwmac4_buf_is_nc()` 跳过 free |
| TX DMA | 软件显示完成、Host 无包 | 描述符 len/PA、tail 与 Zephyr 不一致 | `lynxi_dwmac4_tx_submit` + `resume_dma_tx` |
| **MTL** | ARP 通、标准 ping 不通；帧长阈值约 64B | MTL TX 未开 Store-and-Forward | `LYNXI_MTL_OP_TSF` @ `MTL_CHAN0_TX_OP` |

**二分验证（MTL TSF）**：`ping -s 18`（线长 60B）通，`ping -s 26`（68B）不通 → 阈值约 64B，与 MTL 阈值模式一致；开 TSF 后标准 ping 8/8 通过。

**验收**（49.81）：`he200_gmac_ping_test.sh` → `PASS: ping 192.168.1.2 成功`。

实板复验命令（MSH）：

```text
ifconfig
list_isr
ping 192.168.1.1
```

Host 侧：`sudo ./scripts/he200_test_env.sh` 后 `ping 192.168.1.2`；带宽用 **iperf v2**（端口 5001），勿用 iperf3/5201。

### 1.3 带宽（2026-06-06）

| 项 | 结果 | 备注 |
|----|------|------|
| ping 8/8 | PASS | `he200_gmac_ping_test.sh` |
| Host→EP UDP iperf | ~52 Mbps | EP `iperf -s -u` |
| EP→Host udpblast | ~177 Mbps | `he200_gmac_iperf_test.sh` EP TX 项 |
| Host→EP TCP iperf | ~0.07 Mbps | **未达标**；GMAC TX/RX 计数正常，疑 lwIP TCP/socket |

**TX 环满（36 包后失败）**：DWMAC4 硬件 tail 停在「下一空槽」；软件须保持 **N-1** 占用（`lynxi_dwmac4_tx_in_use`），`TRANSMIT_DESC_SIZE=64`，禁止在 `set_tx_qptr` 写 tail（仅 `resume_dma_tx`）。全环扫描 `lynxi_dwmac4_tx_reclaim_all()` 回收完成描述符。

## 2. 当前代码中的诊断日志

文件：`synopGMAC.c`

### 2.1 初始化 PHY 诊断

日志前缀：

- `gmac phy[init]: ...`

字段说明：

- `addr`：PHY 地址
- `id`：`PHYID1:PHYID2`
- `bmcr`：基础控制寄存器
- `bmsr`：基础状态寄存器
- `anar`：本端自协商广告
- `anlpar`：对端返回能力
- `stat1000`：千兆协商结果
- `ctl1000`：千兆能力广告控制
- `estatus`：扩展能力状态
- `link`：链路状态位（由 `BMSR_LSTATUS` 解析）
- `aneg_en`：是否启用自协商
- `aneg_done`：自协商是否完成

### 2.2 BMSR 变化日志（插拔感知）

日志前缀：

- `gmac phy[bmsr-change]: old -> new (link=.. aneg_done=..)`

说明：

- 仅在 `BMSR` 发生变化时打印，避免刷屏。
- 可用于判断 PHY 是否感知到网线插拔/链路状态变化。

## 3. 已观测到的关键日志

示例：

`gmac phy[init]: addr=1 id=001c:c916 bmcr=1040 bmsr=7989 anar=01e1 anlpar=0000 stat1000=0000 ctl1000=0200 estatus=2000 link=0 aneg_en=1 aneg_done=0`

解读：

- `id=001c:c916`：MDIO 可通信，PHY 可被识别。
- `ctl1000=0200`：本端已宣告 1000M Full 能力。
- `aneg_en=1`、`aneg_done=0`：自协商未完成。
- `anlpar=0000`：未收到对端能力页。

结论：更偏向物理层链路问题（线缆/接口/变压器/PHY 侧硬件路径/时钟复位），而非 IP 协议栈问题。

## 4. 推荐调试步骤

1. 仅上电，不插网线，记录一次 `gmac phy[init]`。
2. 插网线，观察是否出现 `gmac phy[bmsr-change]`。
3. 拔网线，再观察一次 `bmsr-change`。
4. Host 侧同时执行：
   - `ethtool enp25s0f1`
   - `ip link show enp25s0f1`
5. 若一直 `Link detected: no` 且设备侧 `anlpar` 始终为 `0000`，优先检查：
   - 网线与对端端口
   - PHY 复位与供电
   - RGMII/RMII 模式匹配
   - 板级变压器与差分线连接
   - pinmux 是否与其他功能冲突

## 5. 备注

- 目前调试重点是“链路建立”而非“IP 路由”。
- 在链路未建立时，ping、ARP、网关配置结论都不可靠。

## 6. GMAC 结构图

### 6.1 软件分层结构

```mermaid
flowchart TB
    U[用户/协议栈<br/>lwIP + netdev + finsh] --> E[RT-Thread eth_device 层]
    E --> D[synopGMAC.c<br/>驱动主流程]
    D --> H[synopGMAC_Dev.c<br/>MAC/DMA寄存器操作]
    D --> P[synopGMAC_plat.c<br/>平台内存/缓存/中断适配]
    D --> M[mii.c + MDIO读写<br/>PHY管理]
    H --> R[GMAC MAC + DMA 硬件]
    M --> PHY[外部PHY芯片]
    R --> PHY
    PHY --> CABLE[RJ45/变压器/网线]
```

### 6.2 关键数据结构

- `struct synopGMACNetworkAdapter`：驱动核心上下文（MII、GMAC 设备、收发状态）。
- `synopGMACdevice`：MAC/DMA 硬件寄存器与描述符队列控制状态。
- `struct rt_eth_dev eth_dev`：RT-Thread 侧网卡对象，注册名 `e0`。
- `struct rt_timer link_timer`：周期性链路检查（`synopGMAC_linux_cable_unplug_function`）。
- `struct rt_timer rx_poll_timer`：接收轮询兜底（配合中断模式）。

## 7. 实现原理

### 7.1 基本工作机制

1. **板级前置配置**：配置 pinmux、CPR/sysctl 时钟门控、PHY reset。
2. **MAC/DMA 初始化**：设置寄存器、描述符队列、DMA 基地址、收发阈值。
3. **PHY 协商**：通过 MDIO 读写 PHY，启动/观察自协商，得到速率和双工。
4. **链路联动**：链路上/下变化时，重新配置 MAC 速率相关控制位。
5. **数据通路**：
   - TX：lwIP pbuf -> 驱动封装 -> DMA 描述符 -> MAC 发包
   - RX：DMA 收包 -> 驱动取包 -> 交给 lwIP/netdev

### 7.2 速率/双工来源

- `synopGMAC_check_phy_init()` 通过 MII 状态判断链路与协商结果。
- 结果回写 `gmacdev->Speed` 与 `gmacdev->DuplexMode`。
- 然后调用 `lynxi_gmac_set_ctrl_by_speed()` 同步 CPR/GMAC 速率控制。

## 8. 初始化流程

```mermaid
sequenceDiagram
    participant B as board init
    participant G as rt_hw_eth_init
    participant P as PHY
    participant E as eth_init
    participant L as link_timer

    B->>G: INIT_DEVICE_EXPORT(rt_hw_eth_init)
    G->>G: pinmux + reset + CPR时钟使能
    G->>G: synopGMAC_attach()
    G->>P: init_phy() (MDIO访问)
    G->>E: eth_device_init("e0")
    E->>E: MAC/DMA/描述符队列初始化
    E->>P: synopGMAC_check_phy_init()
    E->>L: 启动link_timer
```

## 9. 收发流程

### 9.1 发送（TX）流程

```mermaid
flowchart LR
    A[lwIP/netdev发送请求] --> B[rt_eth_tx]
    B --> C[填充TX描述符]
    C --> D[启动DMA发送]
    D --> E[MAC发出以太网帧]
    E --> F[中断/轮询回收描述符]
```

### 9.2 接收（RX）流程

```mermaid
flowchart LR
    A[PHY收到帧] --> B[MAC写入RX DMA]
    B --> C[RX中断或poll定时器]
    C --> D[驱动取包并组装pbuf]
    D --> E[eth_device_ready]
    E --> F[lwIP协议栈处理]
```

## 10. DWMAC4 数据通路（2026-06-06）

KA200 使用 **DWC Ethernet v4.x**（非 legacy synopGMAC CSR）。关键与 Zephyr `eth_dwmac_lynxi_ka200.c` 对齐：

| 寄存器/字段 | Legacy 误用 | DWMAC4 正确 |
|-------------|-------------|-------------|
| `MAC_CONF` | `GmacConfig` @ `0x0000` 位定义 | @ `0x0000`：`RE/TE/CST/DM` |
| `PKT_FILTER` | `GmacFrameFilter` @ `0x0004` | @ `0x0008` |
| `MAC_ADDR0` | @ `0x0040` | @ `0x0300/0x0304` + `AE` |
| DMA 环地址 | 32 位 `CH_*_LIST` | `CH_*_LIST_H` + `CH_*_LIST`；**`SYSBUS EAME`** |
| 描述符 | 8 字 status/length | 16B `des0..3`；`des0/des1` = 缓冲 PA 低/高 32 位 |
| HW 环内存 | heap/cacheable | nocache `MEM_PADDR_START+MEM_CACHE_SZ+0x200000` |

**实板验收命令**（49.81，先 [`he200_test_env.sh`](../../scripts/he200_test_env.sh)）：

```bash
ip -4 addr show enp25s0f1    # 必须仅有 192.168.1.1/24
sudo stty -F /dev/ttyUSB0 115200 raw -echo
sudo timeout 30 cat /dev/ttyUSB0 > /tmp/he200.log &
hotplug_wdt.sh --devid 0
sleep 20
sudo tcpdump -i enp25s0f1 -n arp or icmp &
ping -c 8 192.168.1.2
```

**典型串口（修复后）**：

```text
gmac: dwmac4 rings tx=36 rx=72 list=0000000900200000/0000000900200240
gmac dwmac4: RxRing[0] des0=0038f038 des1=00000008 des3=c1000000
gmac dwmac4: SYSBUS=00001801 ... CH_STATUS=00000000
Link is up in FULL DUPLEX mode
Link is with 1000M Speed
```

**勿再出现**：`RX halt st=00303100`（RPS+FBE）、`Data abort fault addr=0x10022801`（`SYSBUS_MODE` 宏偏移/初值同名）。

**勿恢复的错误做法**：

- poll/IRQ 内 `tx_reclaim` 与 `rt_eth_tx` 并行回收
- `plat_free_memory` 释放 `lynxi_dwmac4_alloc_nc` 缓冲
- DWMAC4 RX 帧长再减 4
- `netifapi_netif_set_link_up` 从 `link_timer` 调用（mutex 崩溃）
- poll 内直接 `netif->input` 或 poll 内 `tx_reclaim`

## 11. 链路检测与状态机

`link_timer` 周期调用 `synopGMAC_linux_cable_unplug_function()`，核心逻辑：

1. 读取 PHY 链路状态（含去抖动计数）。
2. 若连续判定 down：设置 `LinkState=0`，打印 `No Link`。
3. 若连续判定 up：重新读取协商结果，更新 MAC 配置并打印速率。
4. 本次调试新增：
   - `gmac phy[init]`：初始化寄存器快照
   - `gmac phy[bmsr-change]`：仅在 BMSR 变化时打印

该机制可用于区分：

- **MDIO不通**（读不出稳定 ID）
- **PHY可见但无物理链路**（`anlpar=0`、`aneg_done=0`）
- **链路抖动**（BMSR 频繁变化）
