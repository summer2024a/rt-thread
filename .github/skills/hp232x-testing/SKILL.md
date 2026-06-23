---
name: hp232x-testing
description: '**WORKFLOW SKILL** — Automated testing and validation for HP232X/KA200 RT-Thread BSP. Use when: need to compile and test HP232X firmware on remote server (192.168.49.81), need to monitor serial output via SSH and verify multi-core boot, need to validate memory layout (IRAM0/IRAM1 constraints), need to run automated integration tests with firmware upgrade and reset, need to test compilation doesn\'t break other BSPs (he200), need to verify PCIe boot header format and image loading. Applies to: HP232X BSP development, RT-Thread embedded testing, ARMv8-A multi-core boot verification, memory layout validation, automated device testing workflows. Provides complete test automation including compile, SSH connect, serial monitor, boot analysis, result logging with retry logic and diagnostic support.'
author: lynxi
version: "1.0"
---

# HP232X Automated Testing Skill

## Overview

This skill provides comprehensive automated testing capabilities for the HP232X BSP development workflow, integrating firmware compilation, remote device management, and automated boot result analysis.

本技能为 HP232X BSP 开发工作流提供全面的自动化测试能力，集成了固件编译、远程设备管理和自动化启动结果分析。

## Quick Start

```bash
# Install dependencies
cd /work/rt-thread/bsp/lynxi/hp232x
pip install -r requirements.txt

# Run automated test (Python framework)
python3 test_framework.py run

# Or use bash framework
./test_framework.sh run

# Quick manual test
./quick_test.sh

# Check results in logs/
ls -lh logs/
```

## Prerequisites

### Hardware Environment
- Test server: 192.168.49.81 (lynxi/1 with sudo)
- Device: Connected as EP device to test server
- Serial: /dev/ttyUSB0 @ 115200 baud
- SMB mount: /mnt/49.20/rt-thread → /work/rt-thread

### Software Environment
- Python 3.6+
- paramiko >= 2.7.0 (SSH)
- pyserial >= 3.4 (Serial)
- aarch64-none-elf-gcc (cross-compiler)

## Test Workflow

```mermaid
graph LR
    A[本地编译 / Build] --> B[SSH连接 / Connect]
    B --> C[验证固件 / Verify]
    C --> D[串口监控 / Monitor]
    D --> E[执行复位 / Reset]
    E --> F[分析结果 / Analyze]
```

### Step-by-Step Process

1. **Compile Firmware** (本地编译)
   - Clean build directory: `rm -rf build`
   - Parallel build: `scons -j$(nproc)`
   - Verify size: Check 200-300KB range
   - Generate PCIe boot header via mkimage.py

2. **SSH Connection** (SSH 连接)
   - Connect to 192.168.49.81
   - Verify SMB mount: /mnt/49.20/rt-thread
   - Check firmware symlink: /lib/firmware/lyn_drv/boot-wrapper.bin

3. **Verify Firmware** (验证固件)
   - Ensure symlink points to mounted file
   - Check file exists and size correct
   - Validate PCIe boot header format (offset 0x1C = 0x20)

4. **Serial Monitoring** (串口监控)
   - Connect to /dev/ttyUSB0 @ 115200
   - Clear buffer to avoid stale data
   - Start background monitoring thread
   - Detect boot patterns: msh />, GIC init, CPU activation

5. **Execute Reset** (执行复位)
   - Run: `sudo lynd_hp run -d 0 -r wdt -o5`
   - Monitor serial output (60s timeout)
   - Capture complete boot log

6. **Analyze Results** (分析结果)
   - Detect shell prompt: `r'msh />'`
   - Check heap initialization: `r'Memory.*heap:\s+(\d+)\s+KB'`
   - Verify GIC setup: `r'GIC.*init'`
   - Count active CPUs: `r'cpu\[(\d+)\]: running'`
   - Flag errors: ERROR, panic, Exception, Fault

## Key Validation Points

### 1. Multi-Core Boot

**Success Criteria**:
- ✅ 8 hardware threads boot successfully
- ✅ GIC interrupt controller initializes
- ✅ Secondary CPUs enter WFE loop
- ✅ MSH shell operational
- ✅ Multi-core task scheduling works

**Detection Pattern**:
```python
# Check in test output
assert '多核启动: 8 个 CPU' in result['boot']['features']
assert 'GIC 中断控制器初始化' in result['boot']['features']
assert result['boot']['success'] == True
```

### 2. Memory Constraints

**Layout Requirements**:

```
IRAM0 (512KB @ 0x04000000):
  0x04000000 - 0x040002D7: .head (696B)
  0x04000800 - 0x0403AFFF: .text + .rodata + .data (~235KB)
  0x0403B000 - 0x0407FFFF: Reserved (~288KB) ✅

IRAM1 (512KB @ 0x100000000):
  0x100000000 - 0x10003FFFF: Reserved (256KB) ✅
  0x100040000 - 0x10007FFFF: Used (256KB)
    - cpu stacks, page tables, .bss, heap, free space
```

**Verification**:
```bash
# Check section layout
aarch64-none-elf-objdump -h rtthread.elf

# Verify no sections in reserved areas
# IRAM1 lower 256KB must remain empty
```

### 3. No Impact on Other Boards

**Verification Steps**:
```bash
# Test HP232X build
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build && scons -j$(nproc)
# ✅ Should succeed

# Test he200 build
cd /work/rt-thread/bsp/lynxi/he200
rm -rf build && scons -j$(nproc)
# ✅ Should succeed (HP232X changes are guarded by #ifdef BSP_USING_HP232X)
```

**Code Guidelines**: All HP232X-specific code must use `#ifdef BSP_USING_HP232X` guard.

## Usage Patterns

### Basic Automated Test

```bash
# Using Python framework
cd /work/rt-thread/bsp/lynxi/hp232x
python3 test_framework.py run

# Using bash framework
./test_framework.sh run

# Quick test (single iteration)
./quick_test.sh

# Python API usage
python3 -c "
from test_framework import TestFramework
framework = TestFramework()
framework.setup()
framework.run(max_iterations=3)
"
```

### Manual Verification Steps

**1. Compilation Check**:
```bash
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build
scons -j1  # Single-threaded for error visibility
/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-size rtthread.elf
```

**2. Firmware Header Check**:
```bash
hexdump rtthread-header.bin | head -3
# Verify offset 0x1C = 0x20 (32 bytes)
```

**3. SSH Connection Test**:
```bash
ssh lynxi@192.168.49.81  # Password: 1
ls -l /lib/firmware/lyn_drv/boot-wrapper.bin
ls -lh /mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin
```

**4. Serial Monitor Test**:
```bash
minicom -D /dev/ttyUSB0 -b 115200
# or
cat /dev/ttyUSB0
# or
screen /dev/ttyUSB0 115200
```

## Debugging Failures

### Common Issues

**Build Failure**:
- Check `logs/test_*.log` for errors
- Manually run `scons -j1` for detailed output
- Verify toolchain: `which aarch64-none-elf-gcc`

**SSH Connection Failed**:
- Test network: `ping 192.168.49.81`
- Manual SSH: `ssh -v lynxi@192.168.49.81`
- Check firewall/port accessibility

**No Serial Output**:
- List serial ports: `ls /dev/ttyUSB*`
- Add user to dialout: `sudo usermod -aG dialout $USER`
- Re-login and retry
- Test with minicom manually

**Boot Failure**:
- Review `logs/boot_output_*.txt`
- Look for error patterns: ERROR, panic, Exception
- Compare with successful boot logs
- Check PCIe boot header format (offset 0x1C)

**Firmware Size Out of Range**:
- Check rtconfig.h for unnecessary features
- Disable DFS, POSIX, LWIP if not needed
- Analyze symbol size: `aarch64-none-elf-nm --size-sort rtthread.elf | tail -20`

## Test Results Structure

### Result JSON Format

```json
{
  "session_id": "20241020_143052",
  "timestamp": "2024-10-20T14:30:52",
  "build": {
    "success": true,
    "size": 254.32
  },
  "ssh": {
    "connected": true,
    "firmware_ok": true
  },
  "boot": {
    "success": true,
    "output_lines": 142,
    "features": [
      "MSH Shell 启动成功",
      "初始化进程堆: 256 KB",
      "GIC 中断控制器初始化",
      "多核启动: 8 个 CPU"
    ],
    "errors": []
  }
}
```

### Log Files

- `logs/test_*.log`: Main test log
- `logs/serial_*.log`: Raw serial data
- `logs/boot_output_*.txt`: Complete boot output
- `logs/result_*.json`: Structured results

## Extension Points

### Custom Boot Patterns

Add to `BootMarkers` dictionary in `test_hp232x.py`:

```python
BOOT_MARKERS = {
    'shell_prompt': r'msh />',
    'custom_feature': r'YourPatternHere',
}
```

### Multi-Device Testing

```python
DEVICES = [
    {'name': 'hp232x-1', 'serial': '/dev/ttyUSB0'},
    {'name': 'hp232x-2', 'serial': '/dev/ttyUSB1'},
]

for device in DEVICES:
    serial_config['port'] = device['serial']
    result = run_test()
    # Log per-device results
```

### Auto Retry Logic

```python
max_retries = 3
for attempt in range(max_retries):
    result = run_test()
    if result['boot']['success']:
        break
    elif attempt < max_retries - 1:
        time.sleep(5)  # Wait before retry
```

## Best Practices

### Iterative Development

```python
# Add debug markers for prototype code
print('P', end='')  # Enter _start
print('A', end='')  # Check CurrentEL
print('.', end='')  # BSS clearing progress
# Remove before commit
```

### Log Comparison

```python
# On failure, compare success vs failure logs
if not result['boot']['success']:
    # Find last successful test
    # Diff the boot outputs
    # Identify divergence point
```

### Test Isolation

```python
# Clean before each test
builder.clean()
serial_monitor.clear_buffer()
```

## Relevant Files

### Test Infrastructure
- `/work/rt-thread/bsp/lynxi/hp232x/test_hp232x.py`: Main test script
- `/work/rt-thread/bsp/lynxi/hp232x/requirements.txt`: Python dependencies
- `/work/rt-thread/bsp/lynxi/hp232x/TEST_METHODOLOGY.md`: Detailed methodology

### HP232X BSP
- `/work/rt-thread/bsp/lynxi/hp232x/HANDOFF_SLIM.md`: Handoff documentation
- `/work/rt-thread/bsp/lynxi/hp232x/Test_env.md`: Environment setup
- `/work/rt-thread/bsp/lynxi/hp232x/link.lds`: Memory layout
- `/work/rt-thread/libcpu/aarch64/cortex-a/entry_point.S`: Boot entry

### Configuration
- `/work/rt-thread/bsp/lynxi/hp232x/rtconfig.h`: RT-Thread config
- `/work/rt-thread/bsp/lynxi/hp232x/drivers/board.h/c/h`: Board initialization

## Command Reference

### Test Script

```bash
# Basic test
python3 test_hp232x.py

# No build clean
python3 test_hp232x.py --no-clean

# Custom log directory
python3 test_hp232x.py --log-dir custom_logs

# Specify parallel jobs
python3 test_hp232x.py --parallel 4
```

### Manual Commands

```bash
# Build
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build
scons -j$(nproc)

# Check size
aarch64-none-elf-size rtthread.elf

# Check sections
aarch64-none-elf-objdump -h rtthread.elf

# Check header
hexdump rtthread-header.bin | head -3

# SSH test
ssh lynxi@192.168.49.81
sudo lynd_hp run -d 0 -r wdt -o5

# Serial test
minicom -D /dev/ttyUSB0 -b 115200
```

## Integration with Development Workflow

### Before Commit

```bash
# Run full test suite
python3 test_hp232x.py

# Verify no impact on other BSPs
cd /work/rt-thread/bsp/lynxi/he200
rm -rf build && scons -j$(nproc)  # Must succeed
```

### After Code Changes

```python
# Automated verification
result = run_test()
if result['boot']['success']:
    # Good to continue
    pass
else:
    # Investigate using logs
    analyze_logs(result)
```

### Continuous Integration

```bash
# In CI pipeline
git checkout $COMMIT_SHA
python3 test_hp232x.py
if [ $? -ne 0 ]; then
    echo "Test failed for commit $COMMIT_SHA"
    exit 1
fi
```

## Support and Documentation

- **Detailed Methodology**: `/work/rt-thread/bsp/lynxi/hp232x/TEST_METHODOLOGY.md`
- **BSP Handoff**: `/work/rt-thread/bsp/lynxi/hp232x/HANDOFF_SLIM.md`
- **Test Environment**: `/work/rt-thread/bsp/lynxi/hp232x/Test_env.md`
- **RT-Thread Docs**: https://www.rt-thread.io/document/site/
- **Python Paramiko**: https://docs.paramiko.org/
- **Python PySerial**: https://pyserial.readthedocs.io/

## Summary

This skill provides:

✅ Automated firmware compilation and size validation
✅ Remote device management via SSH
✅ Real-time serial monitoring and capture
✅ Intelligent boot pattern detection
✅ Structured logging and result analysis
✅ Multi-core boot verification (GIC support)
✅ Memory constraint validation (IRAM0/IRAM1)
✅ Ensures no impact on other BSPs
✅ Extensible architecture for custom testing

**Key Validation Tasks**:
1. Multi-core support with GIC → OK
2. Boot to MSH shell → OK
3. Memory layout compliance → OK
4. No dependency on other BSPs → OK

**Target Success Metrics**:
- Firmware size: 200-300KB
- Boot time: < 10s to shell
- All 8 CPUs active
- Zero errors in boot log
- Stable shell operation