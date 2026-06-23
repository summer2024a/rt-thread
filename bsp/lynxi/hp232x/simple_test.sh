#!/bin/bash
# HP232X Simple Test Script
# Basic connectivity and firmware update test

set -e

echo "=== HP232X Basic Test ==="
echo "Work directory: $(pwd)"

# Test 1: SSH Connection
echo ""
echo "[1/3] Testing SSH connection..."
sshpass -p '1' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
  lynxi@192.168.49.81 \
  "echo 'SSH connection OK' && date && \
   ls -l /lib/firmware/lyn_drv/boot-wrapper.bin && \
   ls -lh /mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin" && \
  echo "✓ SSH and firmware paths OK"
echo ""

# Test 2: Firmware update
echo "[2/3] Triggering firmware update..."
sshpass -p '1' ssh lynxi@192.168.49.81 'sudo -S lynd_hp run -d 0 -r wdt -o5' << EOF
1
EOF
echo "✓ Firmware update command executed"
echo ""

# Test 3: Monitor serial (30 seconds)
echo "[3/3] Monitoring serial output (30 seconds)..."
timeout 30 cat /dev/ttyUSB0 | tee -a test_serial_$(date +%Y%m%d_%H%M%S).log

echo ""
echo "=== Test Complete ==="
echo "Check the log file for details"
ls -lh test_serial_*.log | tail -1