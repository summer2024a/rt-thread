#!/bin/bash
# Quick test script for development testing

echo "=== HP232X Quick Test ==="
echo "1. Compiling firmware..."
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build
scons -j$(nproc)

if [ $? -ne 0 ]; then
    echo "✗ Compilation failed"
    exit 1
fi

echo "✓ Compilation successful"
echo "2. Firmware size:"
ls -lh rtthread-header.bin

echo "3. Triggering firmware update..."
sshpass -p '1' ssh lynxi@192.168.49.81 'sudo lynd_hp run -d 0 -r wdt -o5' << EOF
1
EOF

echo "✓ Firmware update triggered"
echo "4. Monitoring serial output (60 seconds)..."
timeout 60 cat /dev/ttyUSB0 | tee -a serial_output_$(date +%Y%m%d_%H%M%S).log

echo "5. Analysis complete"