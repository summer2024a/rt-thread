#!/usr/bin/env bash
# HE200 EP 实板调试环境准备（在 192.168.49.81 上运行）
set -euo pipefail

HOST_IF="${HOST_IF:-enp25s0f1}"
HOST_IP="${HOST_IP:-192.168.1.1}"
HOST_PREFIX="${HOST_PREFIX:-24}"
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
FIRMWARE_LINK="${FIRMWARE_LINK:-/lib/firmware/lynd_pcie/u-boot.bin}"
HOTPLUG_SCRIPT="${HOTPLUG_SCRIPT:-/mnt/49.20/tools/drivers_test/periph_slv_test/hotplug_wdt.sh}"

echo "=== HE200 测试环境检查 ==="

if ! ip link show "$HOST_IF" &>/dev/null; then
    echo "ERROR: 网口 $HOST_IF 不存在"
    exit 1
fi

sudo ip link set "$HOST_IF" up
sudo ip addr flush dev "$HOST_IF" 2>/dev/null || true
sudo ip addr add "${HOST_IP}/${HOST_PREFIX}" dev "$HOST_IF"

echo "--- $HOST_IF ---"
ip addr show "$HOST_IF"
ethtool "$HOST_IF" 2>/dev/null | grep -E 'Link detected|Speed|Duplex' || true

if [[ -e "$SERIAL_DEV" ]]; then
    echo "串口: $SERIAL_DEV ($(ls -l "$SERIAL_DEV"))"
else
    echo "WARN: 串口 $SERIAL_DEV 不存在"
fi

if [[ -L "$FIRMWARE_LINK" || -f "$FIRMWARE_LINK" ]]; then
    echo "固件: $(ls -l "$FIRMWARE_LINK")"
else
    echo "WARN: 固件 $FIRMWARE_LINK 不存在"
fi

if [[ -x "$HOTPLUG_SCRIPT" ]]; then
    echo "热插拔: $HOTPLUG_SCRIPT --devid 0"
else
    echo "WARN: 热插拔脚本 $HOTPLUG_SCRIPT 不可执行"
fi

echo "=== 完成。EP 侧 IP 应为 192.168.1.2，验收: ping 192.168.1.2 ==="
