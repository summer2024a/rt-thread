# HE200 EP 实板调试指南

本文档汇总 HE200 EP 设备在 Lynxi 服务器环境下的调试方法，与 [`测试环境.md`](测试环境.md)、[`移植记录.md`](移植记录.md) §8 配套使用。

## 1. 环境拓扑

| 角色 | 地址/设备 | 说明 |
|------|-----------|------|
| 调试服务器 | `192.168.49.81`，用户 `lynxi` | SSH 编译、烧录、Host 网口 |
| EP 串口 | `/dev/ttyUSB0` @ 115200 | `screen /dev/ttyUSB0 115200` |
| 直连网口 | 服务器 `enp25s0f1` ↔ EP RJ45 | 测试网段 **192.168.1.0/24** |
| Host IP | `192.168.1.1/24` | 与 EP `192.168.1.2` 同网段 |
| 固件软链 | `/lib/firmware/lynd_pcie/u-boot.bin` | 指向 `rtthread.bin` 或 `zephyr.bin` |

**注意**：`192.168.49.81` 是服务器管理 IP，与 GMAC 测试网段独立。

## 2. 环境准备

在 `192.168.49.81` 执行：

```bash
cd /mnt/49.20/rt-thread/bsp/lynxi/he200
sudo ./scripts/he200_test_env.sh
```

或手动：

```bash
sudo ip link set enp25s0f1 up
sudo ip addr add 192.168.1.1/24 dev enp25s0f1
ethtool enp25s0f1   # 确认 Link detected: yes
```

## 3. 固件更新（热插拔）

```bash
# 确认软链
ls -l /lib/firmware/lynd_pcie/u-boot.bin

# RT-Thread 示例
ln -sf /mnt/49.20/rt-thread/bsp/lynxi/he200/rtthread.bin /lib/firmware/lynd_pcie/u-boot.bin

# Zephyr 示例
ln -sf /work/zephyr-rtos/zephyrproject/build_he200_ep_gmac/zephyr/zephyr.bin /lib/firmware/lynd_pcie/u-boot.bin

# 热插拔加载
/mnt/49.20/tools/drivers_test/periph_slv_test/hotplug_wdt.sh --devid 0
```

## 4. RT-Thread 编译

```bash
cd /mnt/49.20/rt-thread/bsp/lynxi/he200
export RTT_CC_PREFIX=/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
scons -j8
```

Kconfig 要点：`BSP_USING_GMAC=y`，**关闭** `BSP_USING_RPMSG_NET`。

## 5. Zephyr 编译

```bash
cd /work/zephyr-rtos/zephyrproject
west build -b he200_ep -d build_he200_ep_gmac app_shell_fs -- \
  -DEXTRA_CONF_FILE=../zephyr/boards/lynxi/he200_ep/he200_ep_gmac.conf
```

## 6. GMAC 调试检查清单

| 检查项 | 正确做法 | 禁止 |
|--------|----------|------|
| ETH IP 复位 | CPR `0x8c` bit0（`LITE_GMAC_R`） | 写 `0x88` bit0（eMMC 复位） |
| probe 时钟 | `gmac_probe_clocks()`：aclk→phy_ref→hclk | probe 写 `0x66f` |
| 速率字 `0x66f` | 链路 up 后写 | init/probe 阶段写 |
| PHY 复位 | RTL8211F BMCR 软复位 | `portd:23` GPIO 硬复位（DDR 挂死） |
| RGMII 时序 | RTL8211F page 0xd08 TX/RX delay（rgmii-id） | 仅 `rgmii` 无 delay |
| IRQ 78 | `LEVEL_HIGH` + CPU0 affinity | 默认 active-low |
| RGMII 线中断 | mask `GmacRgmiiIntMask` | 开启线中断 → IRQ 风暴 |

## 7. 串口交互与验收

### RT-Thread（MSH）

```text
ifconfig
list_isr          # e0_isr counter 应递增
ping 192.168.1.1
```

预期日志：

```text
gmac: probe clocks CPR+0x8c=0x00000207
gmac: BMCR soft reset (PHYID=0x001c, skip gpio portd:23)
gmac: RTL8211F rgmii-id delay configured
gmac phy[init]: addr=1 id=001c:c916 ... anlpar=xxxx (插网线后非 0000)
```

### Zephyr（Shell）

```text
net iface
net stats
ping 192.168.1.1
```

预期 `printk`：

```text
he200 PHY: RTL8211F rgmii-id delay configured
he200 GMAC: PHY ID 001c:c916 (0x001cc916)
```

### Host 侧

```bash
ping 192.168.1.2
iperf3 -s    # 带宽压测服务端
```

## 8. 故障树

```text
PHY 识别 (id=001c:c916)
  └─ 失败 → 查时钟门控 gmac_probe_clocks、MDC 分频
物理链路 (Host Link detected / anlpar)
  └─ 失败 → 查网线、rgmii-id delay、0x66f 时机
IRQ/RX (e0_isr counter / net stats RX)
  └─ 失败 → 查 IRQ 电平、RGMII mask、DMA SWR
IP/ping
  └─ 失败 → 查 192.168.1.x 同网段、ARP、防火墙
```

## 9. 参考文档

- [`移植记录.md`](移植记录.md) §8 — RT GMAC 状态
- [`drivers/net/gmac/DEBUG.md`](drivers/net/gmac/DEBUG.md) — 详细调试记录
- `zephyr/boards/lynxi/PORTING.md` §10.6 — Zephyr GMAC 状态
- Linux：`lynchip-lite-evb.dts`（`phy-mode=rgmii-id`）、`drivers/net/phy/realtek.c`
