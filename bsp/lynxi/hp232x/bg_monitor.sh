#!/bin/bash
# HP232X Remote Test - Background Serial Monitoring Script
# 使用方法：nohup ./bg_monitor.sh &

SERIAL="/dev/ttyUSB0"
LOG_DIR="/tmp/hp232x_logs"
LOG_FILE="$LOG_DIR/bg_serial_$(date +%Y%m%d_%H%M%S).log"

mkdir -p "$LOG_DIR"

echo "=== 后台串口监控 ==="
echo "串口: $SERIAL"
echo "日志: $LOG_FILE"
echo ""

# 后台监控
cat "$SERIAL" > "$LOG_FILE" 2>&1 &
MONITOR_PID=$!

echo "监控PID: $MONITOR_PID"
echo "日志路径: $LOG_FILE"
echo ""
echo "使用以下命令监控实时输出："
echo "  tail -f $LOG_FILE"
echo ""
echo "停止监控："
echo "  kill $MONITOR_PID"
echo ""
echo "查找所有监控进程："
echo "  ps aux | grep 'cat /dev/ttyUSB0' | grep -v grep"

# 返回PID
echo $MONITOR_PID