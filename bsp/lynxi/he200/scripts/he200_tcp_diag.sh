#!/usr/bin/env bash
# TCP Host->EP 诊断：tcpdump 统计 + iperf
set -uo pipefail
HOST_IF="${HOST_IF:-enp25s0f1}"
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyUSB0}"

stty -F "$SERIAL_DEV" 115200 cs8 raw -echo 2>/dev/null || true
printf 'iperf --stop\n' >"$SERIAL_DEV"; sleep 6
printf 'iperf -s\n' >"$SERIAL_DEV"; sleep 5

PCAP=/tmp/he200_tcp_diag.pcap
timeout 10 sudo tcpdump -i "$HOST_IF" -n 'host 192.168.1.2 and tcp port 5001' -w "$PCAP" 2>/dev/null &
TD=$!
sleep 1
iperf -c 192.168.1.2 -p 5001 -t 6 -f m 2>&1 | tee /tmp/he200_tcp_diag_iperf.log || true
wait "$TD" 2>/dev/null || true

echo "=== Host iperf ==="
tail -4 /tmp/he200_tcp_diag_iperf.log 2>/dev/null || true
echo "=== tcpdump ==="
if [[ -f "$PCAP" ]]; then
    tcpdump -nn -r "$PCAP" 2>/dev/null | awk '
    /\.1\.1.*>.*\.1\.2/ {h2e++}
    /\.1\.2.*>.*\.1\.1/ {e2h++}
    /\.1\.2.*>.*\.1\.1.*\.[^P]/ {ack++}
    /\.1\.1.*>.*\.1\.2.*Flags \[P/ {pdata++}
    END{printf "host->ep=%d ep->host=%d ep_pure_ack=%d host_push=%d\n", h2e,e2h,ack,pdata}'
else
    echo "no pcap"
fi
printf 'iperf --stop\n' >"$SERIAL_DEV"
