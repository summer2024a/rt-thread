#!/bin/bash
# HP232X Remote Serial Test
# Read serial output from remote test server

set -e

echo "=== HP232X 远程串口测试 ==="

REMOTE="lynxi@192.168.49.81"
SERIAL="/dev/ttyUSB0"
BAUD=115200

# 串口在测试服务器上！
echo ""
echo "[1/2] 触发固件更新..."
sshpass -p '1' ssh $REMOTE 'sudo lynd_hp run -d 0 -r wdt -o5' <<< '1'
echo "✓ 设备复位命令已执行"
sleep 3

echo ""
echo "[2/2] 监控远程串口输出（60秒）..."
echo "========================================="
sshpass -p '1' ssh $REMOTE "sudo timeout 60 cat $SERIAL" <<< '1' | tee test_serial_$(date +%Y%m%d_%H%M%S).log

echo "========================================="
echo ""
echo "测试完成！查看日志："
ls -lh test_serial_*.log | tail -1

echo ""
echo "分析启动字符："
if [ -f test_serial_$(date +%Y%m%d_%H%M%S).log ]; then
    tr -d '\n' < test_serial_$(date +%Y%m%d_%H%M%S).log | grep -oE '[PEX.]+' && echo ""
fi