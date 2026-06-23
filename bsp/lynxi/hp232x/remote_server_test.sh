#!/bin/bash
# HP232X Remote Test Script for Remote Server
# Run this directly on the test server (192.168.49.81)

set -e

echo "=== HP232X 远程串口测试（服务器端脚本）==="
echo "执行者: $(whoami)"
echo "时间: $(date)"

SMB_MOUNT="/mnt/49.20/rt-thread/bsp/lynxi/hp232x"
SERIAL="/dev/ttyUSB0"

# 测试串口访问
echo ""
echo "[1/3] 检查串口权限..."
if [ -r "$SERIAL" ]; then
    echo "✓ 可以读取 $SERIAL"
else
    echo "需要管理员..."
    if [ "$EUID" -ne 0 ]; then
        echo "请使用 sudo 执行此脚本"
        exit 1
    fi
fi

# 复位设备
echo ""
echo "[2/3] 触发固件更新..."
lynd_hp run -d 0 -r wdt -o5
echo "✓ 复位完成"

# 监控串口
echo ""
echo "[3/3] 监控串口输出（60秒）..."
echo "========================================="
timeout 60 cat "$SERIAL" | tee /tmp/hp232x_serial_$(date +%Y%m%d_%H%M%S).log
echo "========================================="

echo ""
echo "分析启动字符："
LOG_FILE=$(ls -t /tmp/hp232x_serial_*.log 2>/dev/null | head -1)
if [ -f "$LOG_FILE" ]; then
    echo "日志文件: $LOG_FILE"
    grep -oE '[PEX.]+' "$LOG_FILE" | head -1 | tr -d '\n' && echo ""
    
    # 检查 MSH shell
    if grep -q "msh" "$LOG_FILE"; then
        echo "✓✓✓ 检测到 MSH Shell ✓✓✓"
    fi
fi

echo ""
echo "=== 测试完成 ==="