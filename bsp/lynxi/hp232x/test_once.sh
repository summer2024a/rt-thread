#!/bin/bash
# HP232X 简化测试脚本 - 先监控串口，再触发复位

set -e

echo "=== HP232X 简化测试 ==="

REMOTE="lynxi@192.168.49.81"
SERIAL="/dev/ttyUSB0"
MONITOR_DURATION=20  # 监控 20 秒

# 创建服务器端脚本
cat > /tmp/hp232x_test_once.sh <<'SCRIPT_EOF'
#!/bin/bash
SERIAL="/dev/ttyUSB0"
MONITOR_DURATION=20

echo "[1/3] 后台启动串口监控 (${MONITOR_DURATION}秒)..."
timeout $MONITOR_DURATION cat "$SERIAL" 2>&1 &
MONITOR_PID=$!
echo "  监控PID: $MONITOR_PID"

sleep 1

echo ""
echo "[2/3] 触发设备复位..."
lynd_hp run -d 0 -r wdt -o5

echo ""
echo "[3/3] 等待监控完成..."
wait $MONITOR_PID 2>/dev/null

echo ""
echo "=== 监控结束 ==="
SCRIPT_EOF

# 上传并执行
echo "上传测试脚本到测试服务器..."
sshpass -p '1' scp -q /tmp/hp232x_test_once.sh $REMOTE:/tmp/
sshpass -p '1' ssh $REMOTE "chmod +x /tmp/hp232x_test_once.sh"

echo ""
echo "执行测试（先监控 ${MONITOR_DURATION}秒，再复位）..."
echo "========================================="
sshpass -p '1' ssh $REMOTE "sudo /tmp/hp232x_test_once.sh"
echo "========================================="

echo ""
echo "测试完成！"