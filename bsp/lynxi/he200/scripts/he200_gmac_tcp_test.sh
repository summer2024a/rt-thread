#!/usr/bin/env bash
# Zephyr/RT TCP 带宽快测（iperf v2 + zperf/MSH iperf）
# 用法: sudo FIRMWARE=zephyr|rt bash scripts/he200_gmac_tcp_test.sh
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    exec sudo -E bash "$0" "$@"
fi

FIRMWARE="${FIRMWARE:-zephyr}"
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
SERIAL_BAUD="${SERIAL_BAUD:-115200}"
ENV_SCRIPT="${ENV_SCRIPT:-$(dirname "$0")/he200_test_env.sh}"
HOTPLUG_SCRIPT="${HOTPLUG_SCRIPT:-/mnt/49.20/tools/drivers_test/periph_slv_test/hotplug_wdt.sh}"
FIRMWARE_LINK="${FIRMWARE_LINK:-/lib/firmware/lynd_pcie/u-boot.bin}"
RT_BIN="${RT_BIN:-/mnt/49.20/rt-thread/bsp/lynxi/he200/rtthread.bin}"
ZEPHYR_BIN="${ZEPHYR_BIN:-/mnt/49.20/zephyr-rtos/zephyrproject/build_he200_ep_final/zephyr/zephyr.bin}"
IPERF_SEC="${IPERF_SEC:-10}"

case "$FIRMWARE" in
rt)     ln -sf "$RT_BIN" "$FIRMWARE_LINK"; BOOT_WAIT=28 ;;
zephyr) ln -sf "$ZEPHYR_BIN" "$FIRMWARE_LINK"; BOOT_WAIT=50 ;;
*) echo "FIRMWARE=rt|zephyr"; exit 1 ;;
esac

echo "=== TCP 带宽测试 $(date '+%F %T') 固件=$FIRMWARE ==="
[[ -x "$ENV_SCRIPT" ]] && bash "$ENV_SCRIPT"
command -v iperf >/dev/null || { echo "需要 iperf v2"; exit 1; }

stty -F "$SERIAL_DEV" "$SERIAL_BAUD" cs8 -cstopb -parenb raw -echo
SERIAL_LOG="/var/tmp/he200_tcp_serial.log"
rm -f "$SERIAL_LOG"
timeout 120 cat "$SERIAL_DEV" >"$SERIAL_LOG" 2>&1 &
SERIAL_PID=$!
sleep 1
bash "$HOTPLUG_SCRIPT" --devid 0
echo "--- 等待 ${BOOT_WAIT}s ---"
sleep "$BOOT_WAIT"

serial_cmd() { printf '%s\n' "$1" >"$SERIAL_DEV"; sleep "${2:-3}"; }

if [[ "$FIRMWARE" == "rt" ]]; then
    serial_cmd "iperf --stop" 5
    echo "--- Test1: Host->EP TCP ---"
    serial_cmd "iperf -s" 6
    TCP_RX=$(iperf -c 192.168.1.2 -p 5001 -t "$IPERF_SEC" -f m 2>&1) || true
    echo "$TCP_RX"
    RX_MBIT=$(echo "$TCP_RX" | awk '/Mbits\/sec/ {print $(NF-1)}' | tail -1)
    serial_cmd "iperf --stop" 6

    echo "--- Test2: EP->Host TCP (iperf -c from EP) ---"
    pkill -9 iperf 2>/dev/null || true
    iperf -s -p 5001 -f m > /tmp/ep_tcp_tx_host.log 2>&1 &
    IP=$!
    sleep 2
    serial_cmd "iperf -c 192.168.1.1 -p 5001 -t 8" 3
    sleep 12
    kill "$IP" 2>/dev/null; wait "$IP" 2>/dev/null || true
    TX_MBIT=$(grep -a Mbits /tmp/ep_tcp_tx_host.log | awk '{print $(NF-1)}' | tail -1 || true)
    echo "--- Host iperf -s ---"; tail -4 /tmp/ep_tcp_tx_host.log 2>/dev/null || true
else
    echo "--- Test1: Host->EP TCP (zperf tcp download 5001) ---"
    serial_cmd "zperf tcp download 5001" 4
    TCP_RX=$(iperf -c 192.168.1.2 -p 5001 -t "$IPERF_SEC" -f m 2>&1) || true
    echo "$TCP_RX"
    RX_MBIT=$(echo "$TCP_RX" | awk '/Mbits\/sec/ {print $(NF-1)}' | tail -1)
    serial_cmd "zperf tcp download stop" 3

    echo "--- Test2: EP->Host TCP (zperf tcp upload) ---"
    pkill -9 iperf 2>/dev/null || true
    iperf -s -p 5001 -f m > /tmp/ep_tcp_tx_host.log 2>&1 &
    IP=$!
    sleep 2
    serial_cmd "zperf tcp upload 192.168.1.1 5001 8 1K" 2
    sleep 14
    kill "$IP" 2>/dev/null; wait "$IP" 2>/dev/null || true
    TX_MBIT=$(grep -a Mbits /tmp/ep_tcp_tx_host.log | awk '{print $(NF-1)}' | tail -1 || true)
    if [[ -z "${TX_MBIT:-}" ]]; then
        TX_MBIT=$(grep -a -iE 'bitrate|Mbps|Gbits' "$SERIAL_LOG" | grep -oE '[0-9]+\.[0-9]+' | tail -1 || true)
    fi
    echo "--- Host iperf -s ---"; tail -6 /tmp/ep_tcp_tx_host.log 2>/dev/null || true
    echo "--- 串口 zperf ---"
    strings "$SERIAL_LOG" | grep -iE 'zperf|tcp|Mbps|bitrate|finished' | tail -10 || true
fi

kill "$SERIAL_PID" 2>/dev/null || true
wait "$SERIAL_PID" 2>/dev/null || true

echo ""
echo "=== 结果 ($FIRMWARE TCP) ==="
echo "Host->EP (RX): ${RX_MBIT:-N/A} Mbits/s"
echo "EP->Host (TX): ${TX_MBIT:-N/A} Mbits/s"
[[ "$FIRMWARE" == "zephyr" ]] && ln -sf "$RT_BIN" "$FIRMWARE_LINK"
