#!/usr/bin/env bash
# HE200 EP GMAC 验收：串口观察 EP 侧 + Host ping/tcpdump
# 用法: sudo bash scripts/he200_gmac_ping_test.sh
# 远程: echo '密码' | sudo -S bash /mnt/49.20/rt-thread/bsp/lynxi/he200/scripts/he200_gmac_ping_test.sh
# 要点: 串口 cat 必须在热插拔之前后台启动，否则会错过 EP 启动日志。
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    exec sudo -E bash "$0" "$@"
fi

HOST_IF="${HOST_IF:-enp25s0f1}"
HOST_IP="${HOST_IP:-192.168.1.1}"
HOST_PREFIX="${HOST_PREFIX:-24}"
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
SERIAL_BAUD="${SERIAL_BAUD:-115200}"
SERIAL_LOG="${SERIAL_LOG:-/var/tmp/he200_serial.log}"
SERIAL_SEC="${SERIAL_SEC:-50}"
PING_CNT="${PING_CNT:-8}"
FIRMWARE_LINK="${FIRMWARE_LINK:-/lib/firmware/lynd_pcie/u-boot.bin}"
# FIRMWARE=rt|zephyr：自动切换软链（zephyr 测完恢复 rt）
FIRMWARE="${FIRMWARE:-rt}"
RT_BIN="${RT_BIN:-/mnt/49.20/rt-thread/bsp/lynxi/he200/rtthread.bin}"
ZEPHYR_BIN="${ZEPHYR_BIN:-/mnt/49.20/zephyr-rtos/zephyrproject/build_he200_ep_gmac/zephyr/zephyr.bin}"
HOTPLUG_SCRIPT="${HOTPLUG_SCRIPT:-/mnt/49.20/tools/drivers_test/periph_slv_test/hotplug_wdt.sh}"
ENV_SCRIPT="${ENV_SCRIPT:-$(dirname "$0")/he200_test_env.sh}"

die() { echo "ERROR: $*" >&2; exit 1; }

serial_stop() {
    if [[ -n "${SERIAL_CAT_PID:-}" ]] && kill -0 "$SERIAL_CAT_PID" 2>/dev/null; then
        kill "$SERIAL_CAT_PID" 2>/dev/null || true
        wait "$SERIAL_CAT_PID" 2>/dev/null || true
    fi
}
trap serial_stop EXIT

serial_start() {
    [[ -e "$SERIAL_DEV" ]] || die "串口 $SERIAL_DEV 不存在"

    rm -f "$SERIAL_LOG"
    # 勿设 min 0 time N：会导致 cat 在无数据时立刻 EOF 退出
    stty -F "$SERIAL_DEV" "$SERIAL_BAUD" cs8 -cstopb -parenb -ixon -ixoff \
        -crtscts raw -echo

    # 热插拔前启动；勿嵌套 sudo（脚本已 root，嵌套 sudo 后台会立刻退出）
    timeout "$SERIAL_SEC" cat "$SERIAL_DEV" >"$SERIAL_LOG" 2>&1 &
    SERIAL_CAT_PID=$!
    sleep 1
    if ! kill -0 "$SERIAL_CAT_PID" 2>/dev/null; then
        die "串口抓取进程未运行 (pid=$SERIAL_CAT_PID)，检查 $SERIAL_DEV 是否被占用"
    fi
    echo "串口抓取已启动: $SERIAL_DEV -> $SERIAL_LOG (pid=$SERIAL_CAT_PID, ${SERIAL_SEC}s)"
}

serial_report() {
    local bytes

    chmod a+r "$SERIAL_LOG" 2>/dev/null || true
    bytes=$(wc -c <"$SERIAL_LOG" 2>/dev/null || echo 0)
    echo ""
    echo "=== EP 串口摘要 ($SERIAL_LOG, ${bytes} bytes) ==="
    if ! test -s "$SERIAL_LOG" 2>/dev/null; then
        echo "WARN: 串口无数据。检查接线、波特率 ${SERIAL_BAUD}、热插拔是否在抓取启动之后执行。"
        return 1
    fi

    echo "--- 关键字命中 (gmac/link/phy/eth/ping/error) ---"
    grep -a -i -E 'gmac|dwmac|link|phy|eth_|e0|arp|ping|CH_STATUS|swr|SYSBUS|RxRing|regs|Data abort|fault|error|Hi, this is RT' \
        "$SERIAL_LOG" 2>/dev/null | tail -100 || true

    echo "--- 串口尾部 (最后 40 行可打印字符) ---"
    strings "$SERIAL_LOG" 2>/dev/null | tail -40
}

echo "=== HE200 GMAC 验收 $(date '+%F %T') ==="

case "$FIRMWARE" in
rt)
    ln -sf "$RT_BIN" "$FIRMWARE_LINK"
    BOOT_WAIT="${BOOT_WAIT:-22}"
    ;;
zephyr)
    ln -sf "$ZEPHYR_BIN" "$FIRMWARE_LINK"
    BOOT_WAIT="${BOOT_WAIT:-50}"
    SERIAL_SEC="${SERIAL_SEC:-70}"
    ;;
*)
    die "FIRMWARE 须为 rt 或 zephyr，当前: $FIRMWARE"
    ;;
esac
echo "固件模式: $FIRMWARE -> $(readlink -f "$FIRMWARE_LINK" 2>/dev/null || readlink "$FIRMWARE_LINK")"

if [[ -x "$ENV_SCRIPT" ]]; then
    bash "$ENV_SCRIPT"
else
    ip link set "$HOST_IF" up
    ip addr flush dev "$HOST_IF" 2>/dev/null || true
    ip addr add "${HOST_IP}/${HOST_PREFIX}" dev "$HOST_IF"
fi

echo "--- 固件 ---"
ls -l "$FIRMWARE_LINK" 2>/dev/null || die "固件软链不存在: $FIRMWARE_LINK"
[[ -x "$HOTPLUG_SCRIPT" ]] || die "热插拔脚本不可执行: $HOTPLUG_SCRIPT"

serial_start

echo "--- 热插拔加载固件 ---"
bash "$HOTPLUG_SCRIPT" --devid 0

echo "--- 等待 EP 启动 ${BOOT_WAIT}s ---"
sleep "$BOOT_WAIT"

if [[ -x "$ENV_SCRIPT" ]]; then
    bash "$ENV_SCRIPT" 2>/dev/null | grep -E 'inet |Link detected' || true
fi
echo "--- 等待 $HOST_IF Link detected: yes (最多 30s) ---"
for _ in $(seq 1 30); do
    if ethtool "$HOST_IF" 2>/dev/null | grep -q 'Link detected: yes'; then
        ethtool "$HOST_IF" 2>/dev/null | grep -E 'Speed|Duplex|Link detected' || true
        break
    fi
    sleep 1
done
if ! ethtool "$HOST_IF" 2>/dev/null | grep -q 'Link detected: yes'; then
    echo "WARN: Host $HOST_IF 链路仍未 up，继续 ping 以便对照串口"
fi

echo "--- Host ping + tcpdump ---"
PING_OUT=$(timeout 14 bash -c "
    tcpdump -i '$HOST_IF' -e -n 'arp or icmp' > /tmp/he200_pcap.log 2>&1 &
    TP=\$!
    sleep 1
    ping -c '$PING_CNT' -W 1 192.168.1.2
    wait \$TP 2>/dev/null || true
" 2>&1) || true
echo "$PING_OUT"
if echo "$PING_OUT" | grep -q ' 0% packet loss'; then
    PING_OK=1
else
    PING_OK=0
fi

echo "--- ip neigh ---"
ip neigh show dev "$HOST_IF" 2>/dev/null || true

echo "--- tcpdump 摘要 ---"
grep -E 'ARP|ICMP|192\.168\.1|00:55:7b' /tmp/he200_pcap.log 2>/dev/null | tail -20 || true

serial_stop
SERIAL_CAT_PID=""
serial_report || true

echo ""
echo "=== 结果 ==="
if [[ "${PING_OK:-0}" -eq 1 ]]; then
    echo "PASS: ping 192.168.1.2 成功"
    if [[ "$FIRMWARE" == "zephyr" ]]; then
        ln -sf "$RT_BIN" "$FIRMWARE_LINK"
        echo "已恢复软链 -> RT: $RT_BIN"
    fi
    exit 0
else
    echo "FAIL: ping 192.168.1.2 未通（见串口与 tcpdump 摘要）"
    if [[ "$FIRMWARE" == "zephyr" ]]; then
        ln -sf "$RT_BIN" "$FIRMWARE_LINK"
        echo "已恢复软链 -> RT: $RT_BIN"
    fi
    exit 1
fi
