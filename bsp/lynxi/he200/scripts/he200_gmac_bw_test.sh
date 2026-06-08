#!/usr/bin/env bash
# HE200 GMAC 带宽快测：UDP RX + EP TX(udpblast)，可选 TCP；支持 rt/zephyr
# 用法: sudo FIRMWARE=rt|zephyr bash scripts/he200_gmac_bw_test.sh
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
FIRMWARE="${FIRMWARE:-rt}"
RT_BIN="${RT_BIN:-/mnt/49.20/rt-thread/bsp/lynxi/he200/rtthread.bin}"
ZEPHYR_BIN="${ZEPHYR_BIN:-/mnt/49.20/zephyr-rtos/zephyrproject/build_he200_ep_final/zephyr/zephyr.bin}"
BOOT_WAIT="${BOOT_WAIT:-}"
IPERF_SEC="${IPERF_SEC:-8}"
UDP_BW="${UDP_BW:-50M}"
RUN_TCP="${RUN_TCP:-0}"
PASS_UDP_MBIT="${PASS_UDP_MBIT:-10}"
PASS_TX_MBIT="${PASS_TX_MBIT:-3}"

die() { echo "ERROR: $*" >&2; exit 1; }

case "$FIRMWARE" in
rt)
    ln -sf "$RT_BIN" "$FIRMWARE_LINK"
    BOOT_WAIT="${BOOT_WAIT:-28}"
    SHELL_PROMPT="msh"
    ;;
zephyr)
    ln -sf "$ZEPHYR_BIN" "$FIRMWARE_LINK"
    BOOT_WAIT="${BOOT_WAIT:-50}"
    SHELL_PROMPT="uart"
    ;;
*)
    die "FIRMWARE 须为 rt 或 zephyr"
    ;;
esac

echo "=== HE200 GMAC 带宽测试 $(date '+%F %T') 固件=$FIRMWARE ==="
[[ -x "$ENV_SCRIPT" ]] && bash "$ENV_SCRIPT"
[[ -x "$HOTPLUG_SCRIPT" ]] || die "热插拔脚本不可执行"
[[ -e "$SERIAL_DEV" ]] || die "串口不存在"
command -v iperf >/dev/null || die "需要 iperf v2"

stty -F "$SERIAL_DEV" "$SERIAL_BAUD" cs8 -cstopb -parenb -ixon -ixoff -crtscts raw -echo
SERIAL_LOG="/var/tmp/he200_bw_serial.log"
rm -f "$SERIAL_LOG"
timeout 150 cat "$SERIAL_DEV" >"$SERIAL_LOG" 2>&1 &
SERIAL_PID=$!
sleep 1
bash "$HOTPLUG_SCRIPT" --devid 0
echo "--- 等待 EP 启动 ${BOOT_WAIT}s ---"
sleep "$BOOT_WAIT"

serial_cmd() {
    printf '%s\n' "$1" >"$SERIAL_DEV"
    sleep "${2:-2}"
}

# RT: iperf/udpblast；Zephyr: zperf（需 NET_ZPERF）
if [[ "$FIRMWARE" == "rt" ]]; then
    serial_cmd "iperf --stop" 5

    echo "--- UDP Host->EP (iperf -s -u) ---"
    serial_cmd "iperf -s -u" 5
    UDP_OUT=$(iperf -u -c 192.168.1.2 -p 5001 -b "$UDP_BW" -t 6 -f m 2>&1) || true
    echo "$UDP_OUT"
    UDP_MBIT=$(echo "$UDP_OUT" | awk '/Mbits\/sec/ {print $(NF-1)}' | tail -1)
    EP_UDP=$(grep -a "iperfd" "$SERIAL_LOG" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | tail -1 || true)
    [[ -n "${EP_UDP:-}" ]] && UDP_MBIT="$EP_UDP"
    serial_cmd "iperf --stop" 6

    if [[ "$RUN_TCP" == "1" ]]; then
        echo "--- TCP Host->EP ---"
        serial_cmd "iperf -s" 5
        TCP_OUT=$(iperf -c 192.168.1.2 -p 5001 -t "$IPERF_SEC" -f m 2>&1) || true
        echo "$TCP_OUT"
        TCP_MBIT=$(echo "$TCP_OUT" | awk '/Mbits\/sec/ {print $(NF-1)}' | tail -1)
        serial_cmd "iperf --stop" 6
    else
        TCP_MBIT="skip"
    fi

    echo "--- EP->Host TX (udpblast) ---"
    pkill -9 iperf 2>/dev/null || true
    sleep 1
    serial_cmd "udpblast 192.168.1.1 5001 8" 2
    sleep 10
    EP_TX_MBIT=$(grep -a -iE 'udpblast total|udpblast:' "$SERIAL_LOG" 2>/dev/null \
        | grep -oE '[0-9]+\.[0-9]+' | tail -1 || true)
else
    echo "--- Zephyr: Host->EP UDP (zperf udp download 5001) ---"
    serial_cmd "zperf udp download 5001" 4
    UDP_OUT=$(iperf -u -c 192.168.1.2 -p 5001 -b "$UDP_BW" -t 6 -f m 2>&1) || true
    echo "$UDP_OUT"
    UDP_MBIT=$(echo "$UDP_OUT" | awk '/Mbits\/sec/ {print $(NF-1)}' | tail -1)
    # EP 侧 zperf 收包速率（串口 Finished 行）
    ZP_EP=$(grep -a -iE 'UDP RX|received|bitrate|Mbps|Gbits' "$SERIAL_LOG" 2>/dev/null | tail -8 || true)
    echo "$ZP_EP"
    EP_UDP=$(grep -a -iE 'bitrate|Mbps|Gbits/sec' "$SERIAL_LOG" 2>/dev/null \
        | grep -oE '[0-9]+\.[0-9]+' | tail -1 || true)
    [[ -n "${EP_UDP:-}" ]] && UDP_MBIT="$EP_UDP"
    TCP_MBIT="n/a"

    echo "--- Zephyr: EP->Host UDP (zperf udp upload) ---"
    pkill -9 iperf 2>/dev/null || true
    sleep 1
    rm -f /tmp/zephyr_host_udp.log
    iperf -s -u -p 5001 -f m > /tmp/zephyr_host_udp.log 2>&1 &
    IP=$!
    sleep 2
    serial_cmd "zperf udp upload 192.168.1.1 5001 8 1K 50M" 2
    sleep 12
    kill "$IP" 2>/dev/null || true
    wait "$IP" 2>/dev/null || true
    EP_TX_MBIT=$(grep -a Mbits /tmp/zephyr_host_udp.log 2>/dev/null | awk '{print $(NF-1)}' | tail -1 || true)
    if [[ -z "${EP_TX_MBIT:-}" ]]; then
        EP_TX_MBIT=$(grep -a -iE 'zperf|upload|bitrate|Mbps|Gbits' "$SERIAL_LOG" 2>/dev/null \
            | grep -oE '[0-9]+\.[0-9]+' | tail -1 || true)
    fi
    echo "--- Host iperf -s -u 摘要 ---"
    tail -5 /tmp/zephyr_host_udp.log 2>/dev/null || true
fi

kill "$SERIAL_PID" 2>/dev/null || true
wait "$SERIAL_PID" 2>/dev/null || true

echo ""
echo "=== 结果 ($FIRMWARE) ==="
echo "UDP Host->EP: ${UDP_MBIT:-N/A} Mbits/s (阈值 ${PASS_UDP_MBIT})"
echo "TCP Host->EP: ${TCP_MBIT:-N/A} Mbits/s"
echo "TX EP->Host:  ${EP_TX_MBIT:-N/A} Mbps (阈值 ${PASS_TX_MBIT})"

cmp_float() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 >= b+0) }'; }
UDP_OK=0; TX_OK=0
[[ -n "${UDP_MBIT:-}" ]] && cmp_float "$UDP_MBIT" "$PASS_UDP_MBIT" && UDP_OK=1
[[ -n "${EP_TX_MBIT:-}" ]] && cmp_float "$EP_TX_MBIT" "$PASS_TX_MBIT" && TX_OK=1

if [[ "$UDP_OK" -eq 1 && "$TX_OK" -eq 1 ]]; then
    echo "PASS: 带宽达标"
    [[ "$FIRMWARE" == "zephyr" ]] && ln -sf "$RT_BIN" "$FIRMWARE_LINK"
    exit 0
fi
echo "FAIL: 带宽未完全达标"
[[ "$FIRMWARE" == "zephyr" ]] && ln -sf "$RT_BIN" "$FIRMWARE_LINK"
exit 1
