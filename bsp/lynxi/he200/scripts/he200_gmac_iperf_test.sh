#!/usr/bin/env bash
# HE200 EP GMAC iperf 验收（RT netutils iperf v2，端口 5001）
# 用法: sudo bash scripts/he200_gmac_iperf_test.sh
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    exec sudo -E bash "$0" "$@"
fi

HOST_IF="${HOST_IF:-enp25s0f1}"
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
SERIAL_BAUD="${SERIAL_BAUD:-115200}"
ENV_SCRIPT="${ENV_SCRIPT:-$(dirname "$0")/he200_test_env.sh}"
HOTPLUG_SCRIPT="${HOTPLUG_SCRIPT:-/mnt/49.20/tools/drivers_test/periph_slv_test/hotplug_wdt.sh}"
FIRMWARE_LINK="${FIRMWARE_LINK:-/lib/firmware/lynd_pcie/u-boot.bin}"
RT_BIN="${RT_BIN:-/mnt/49.20/rt-thread/bsp/lynxi/he200/rtthread.bin}"
BOOT_WAIT="${BOOT_WAIT:-28}"
IPERF_SEC="${IPERF_SEC:-10}"
UDP_BW="${UDP_BW:-50M}"
PASS_TCP_MBIT="${PASS_TCP_MBIT:-3}"
PASS_UDP_MBIT="${PASS_UDP_MBIT:-10}"

die() { echo "ERROR: $*" >&2; exit 1; }

echo "=== HE200 GMAC iperf 验收 $(date '+%F %T') ==="
ln -sf "$RT_BIN" "$FIRMWARE_LINK"
[[ -x "$ENV_SCRIPT" ]] && bash "$ENV_SCRIPT"
[[ -x "$HOTPLUG_SCRIPT" ]] || die "热插拔脚本不可执行: $HOTPLUG_SCRIPT"
[[ -e "$SERIAL_DEV" ]] || die "串口 $SERIAL_DEV 不存在"
command -v iperf >/dev/null || die "需要 iperf v2（非 iperf3）"

stty -F "$SERIAL_DEV" "$SERIAL_BAUD" cs8 -cstopb -parenb -ixon -ixoff -crtscts raw -echo
SERIAL_LOG="/var/tmp/he200_iperf_serial.log"
rm -f "$SERIAL_LOG"
timeout 120 cat "$SERIAL_DEV" >"$SERIAL_LOG" 2>&1 &
SERIAL_PID=$!
sleep 1
bash "$HOTPLUG_SCRIPT" --devid 0
echo "--- 等待 EP 启动 ${BOOT_WAIT}s ---"
sleep "$BOOT_WAIT"

serial_cmd() {
    printf '%s\n' "$1" >"$SERIAL_DEV"
    sleep "${2:-3}"
}

serial_stop_iperf() {
    serial_cmd "iperf --stop" 10
    serial_cmd "iperf --stop" 3
}

serial_stop_iperf

echo "--- Test1: Host->EP TCP (EP iperf -s) ---"
serial_cmd "iperf -s" 8
TCP_OUT=$(iperf -c 192.168.1.2 -p 5001 -t "$IPERF_SEC" -f m 2>&1) || true
echo "$TCP_OUT"
TCP_MBIT=$(echo "$TCP_OUT" | awk '/Mbits\/sec/ {print $(NF-1)}' | tail -1)
serial_stop_iperf

echo "--- Test2: Host->EP UDP (EP iperf -s -u) ---"
serial_cmd "iperf -s -u" 5
UDP_OUT=$(iperf -u -c 192.168.1.2 -p 5001 -b "$UDP_BW" -t 5 -f m 2>&1) || true
echo "$UDP_OUT"
UDP_MBIT=$(echo "$UDP_OUT" | awk '/Mbits\/sec/ {print $(NF-1)}' | tail -1)
# EP 侧 UDP server 实测（串口 iperfd Mbps）
EP_UDP_MBIT=$(grep -a "iperfd" "$SERIAL_LOG" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | tail -1 || true)
[[ -n "${EP_UDP_MBIT:-}" ]] && UDP_MBIT="$EP_UDP_MBIT"
serial_stop_iperf

echo "--- Test3: EP->Host TX (udpblast) ---"
pkill -9 iperf 2>/dev/null || true
sleep 1
serial_cmd "udpblast 192.168.1.1 5001 8" 3
sleep 10
EP_TX_MBIT=$(grep -a "udpblast total" "$SERIAL_LOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+\.[0-9]+' | head -1 || true)
echo "EP udpblast: ${EP_TX_MBIT:-N/A} Mbps (串口日志)"

kill "$SERIAL_PID" 2>/dev/null || true
wait "$SERIAL_PID" 2>/dev/null || true
echo "--- EP 串口 iperf 日志 ---"
grep -a -iE 'iperf|Mbps|Gbps|Connect' "$SERIAL_LOG" 2>/dev/null | tail -15 || true

cmp_float() {
    awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 >= b+0) }'
}

TCP_OK=0; UDP_OK=0; EPTX_OK=0
[[ -n "${TCP_MBIT:-}" ]] && cmp_float "$TCP_MBIT" "$PASS_TCP_MBIT" && TCP_OK=1
[[ -n "${UDP_MBIT:-}" ]] && cmp_float "$UDP_MBIT" "$PASS_UDP_MBIT" && UDP_OK=1

echo ""
echo "=== 结果 ==="
echo "TCP Host->EP: ${TCP_MBIT:-N/A} Mbits/s (阈值 ${PASS_TCP_MBIT})"
echo "UDP Host->EP: ${UDP_MBIT:-N/A} Mbits/s (阈值 ${PASS_UDP_MBIT})"
echo "TX EP->Host (udpblast): ${EP_TX_MBIT:-N/A} Mbps (阈值 ${PASS_TCP_MBIT})"
EPTX_OK=0
[[ -n "${EP_TX_MBIT:-}" ]] && cmp_float "$EP_TX_MBIT" "$PASS_TCP_MBIT" && EPTX_OK=1
if [[ "$UDP_OK" -eq 1 && "$EPTX_OK" -eq 1 ]]; then
    echo "PASS: iperf/udpblast 带宽达标 (UDP RX + EP TX)"
    exit 0
fi
echo "FAIL: 带宽未达标（见上方输出）"
exit 1
