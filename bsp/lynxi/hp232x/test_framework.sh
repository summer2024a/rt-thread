#!/bin/bash
# HP232X Automated Testing Framework
# This script automates compilation, firmware update, and serial monitoring

set -e

# Configuration
TEST_SERVER="192.168.49.81"
SERVER_USER="lynxi"
SERVER_PASS="1"
SERIAL_PORT="/dev/ttyUSB0"
BAUD_RATE="115200"
LOCAL_FW_PATH="/work/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin"
REMOTE_FW_BASE="/mnt/49.20/rt-thread/bsp/lynxi/hp232x"
REMOTE_FW_LINK="/lib/firmware/lyn_drv/boot-wrapper.bin"
UPGRADE_CMD="sudo lynd_hp run -d 0 -r wdt -o5"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging
LOG_DIR="./test_logs"
mkdir -p "$LOG_DIR"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
LOG_FILE="$LOG_DIR/test_${TIMESTAMP}.log"

# Function to log messages
log() {
    local level=$1
    shift
    local message="$@"
    local color=""
    case $level in
        INFO) color="$BLUE" ;;
        SUCCESS) color="$GREEN" ;;
        WARNING) color="$YELLOW" ;;
        ERROR) color="$RED" ;;
    esac
    echo -e "${color}[$(date '+%Y-%m-%d %H:%M:%S')] [$level] $message${NC}" | tee -a "$LOG_FILE"
}

# Function to read serial output
read_serial() {
    local timeout=60
    log INFO "Reading serial output ($timeout seconds timeout)..."
    
    timeout $timeout cat "$SERIAL_PORT" | tee -a "$LOG_FILE" | while IFS= read -r line; do
        echo "$line"
        
        # Check for successful patterns
        if echo "$line" | grep -q "msh"; then
            log SUCCESS "✓ MSH shell detected!"
            return 0
        fi
        
        # Check for failure patterns
        if echo "$line" | grep -q "ERROR\|PANIC\|exception\|Exception"; then
            log WARNING "⚠ Error or exception detected in output"
        fi
    done || {
        log WARNING "Serial read timed out after $timeout seconds"
        return 1
    }
    
    return 0
}

# Function to reset device
reset_device() {
    log INFO "Resetting device to update firmware..."
    sshpass -p "$SERVER_PASS" ssh "$SERVER_USER@$TEST_SERVER" "$UPGRADE_CMD" 2>&1 | tee -a "$LOG_FILE"
    
    if [ $? -eq 0 ]; then
        log SUCCESS "✓ Device reset command executed"
        sleep 3  # Wait for reset to complete
    else
        log ERROR "✗ Device reset failed"
        return 1
    fi
}

# Function to compile firmware
compile_firmware() {
    log INFO "Compiling RT-Thread firmware..."
    cd /work/rt-thread/bsp/lynxi/hp232x
    
    # Clean and build
    rm -rf build
    scons -j$(nproc) 2>&1 | tee -a "$LOG_FILE"
    
    if [ $? -eq 0 ] && [ -f "rtthread-header.bin" ]; then
        log SUCCESS "✓ Firmware compiled successfully"
        
        # Check firmware size
        local size=$(stat -f%z "rtthread-header.bin" 2>/dev/null || stat -c%s "rtthread-header.bin")
        log INFO "Firmware size: $size bytes"
        
        # Verify within target range (200-300KB)
        if [ $size -lt 204800 ] || [ $size -gt 307200 ]; then
            log WARNING "⚠ Firmware size $size bytes outside target range (200-300KB)"
        else
            log SUCCESS "✓ Firmware size within target range"
        fi
        
        return 0
    else
        log ERROR "✗ Firmware compilation failed"
        return 1
    fi
}

# Function to analyze startup sequence
analyze_startup() {
    log INFO "Analyzing startup sequence from log..."
    
    local log_file="$LOG_DIR/test_${TIMESTAMP}.log"
    
    # Extract startup patterns
    echo "=== Startup Analysis ===" | tee -a "$LOG_FILE"
    
    # Check for character patterns (P, E, X, etc.)
    if grep -o '[PEX.]' "$log_file" | head -100 > /tmp/startup_chars.txt; then
        echo "Startup character sequence:" | tee -a "$LOG_FILE"
        cat /tmp/startup_chars.txt | tr -d '\n' | head -c 200 | tee -a "$LOG_FILE"
        echo "" | tee -a "$LOG_FILE"
        
        # Analyze pattern
        local p_count=$(grep -o 'P' /tmp/startup_chars.txt | wc -l)
        local e_count=$(grep -o 'E' /tmp/startup_chars.txt | wc -l)
        local x_count=$(grep -o 'X' /tmp/startup_chars.txt | wc -l)
        local dot_count=$(grep -o '\.' /tmp/startup_chars.txt | wc -l)
        
        echo "" | tee -a "$LOG_FILE"
        echo "Pattern analysis:" | tee -a "$LOG_FILE"
        echo "  - P (primary CPU): $p_count" | tee -a "$LOG_FILE"
        echo "  - E (exception level): $e_count" | tee -a "$LOG_FILE"
        echo "  - X (expected stop): $x_count" | tee -a "$LOG_FILE"
        echo "  - . (BSS progress): $dot_count (≈$(($dot_count * 64)) bytes)" | tee -a "$LOG_FILE"
    fi
    
    # Check for specific error messages
    echo "" | tee -a "$LOG_FILE"
    echo "Error messages:" | tee -a "$LOG_FILE"
    grep -i "error\|exception\|panic\|fault" "$log_file" | tail -10 | tee -a "$LOG_FILE"
    
    # Extract RT-Thread version and boot messages
    echo "" | tee -a "$LOG_FILE"
    grep -i "RT-Thread\|\\ |\\ \|version" "$log_file" | head -5 | tee -a "$LOG_FILE"
}

# Main test loop
main() {
    log INFO "=========================================="
    log INFO "HP232X Automated Testing Framework"
    log INFO "=========================================="
    
    local max_iterations=10
    local iteration=0
    
    while [ $iteration -lt $max_iterations ]; do
        iteration=$((iteration + 1))
        log INFO "=========================================="
        log INFO "Test Iteration $iteration of $max_iterations"
        log INFO "=========================================="
        
        # Step 1: Compile firmware
        if ! compile_firmware; then
            log ERROR "Compilation failed, stopping test loop"
            exit 1
        fi
        
        # Step 2: Reset device (triggers firmware update via symlink)
        if ! reset_device; then
            log ERROR "Reset failed, but continuing to monitor serial..."
        fi
        
        # Step 3: Monitor serial output
        sleep 1  # Give device time to start
        read_serial
        local serial_result=$?
        
        # Step 4: Analyze results
        analyze_startup
        
        # Check if MSH shell is up
        if grep -q "msh" "$LOG_DIR/test_${TIMESTAMP}.log"; then
            log SUCCESS "=========================================="
            log SUCCESS "✓✓✓ MSH SHELL SUCCESSFULLY STARTED ✓✓✓"
            log SUCCESS "=========================================="
            exit 0
        else
            log WARNING "MSH shell not detected in this iteration"
            sleep 2
        fi
    done
    
    log ERROR "=========================================="
    log ERROR "Testing failed after $max_iterations iterations"
    log ERROR "=========================================="
    exit 1
}

# Configure serial port
setup_serial() {
    log INFO "Configuring serial port $SERIAL_PORT..."
    if [ -c "$SERIAL_PORT" ]; then
        stty -F "$SERIAL_PORT" $BAUD_RATE cs8 -cstopb -parenb
        log SUCCESS "✓ Serial port configured"
    else
        log ERROR "Serial port $SERIAL_PORT not found"
        exit 1
    fi
}

# Install dependencies
check_dependencies() {
    log INFO "Checking dependencies..."
    
    local deps=("sshpass" "timeout" "stty")
    for dep in "${deps[@]}"; do
        if ! command -v "$dep" &> /dev/null; then
            log WARNING "Missing dependency: $dep"
            log INFO "Installing: $dep"
            sudo apt-get install -y sshpass coreutils 2>&1 | tee -a "$LOG_FILE"
        fi
    done
    
    log SUCCESS "✓ Dependencies checked"
}

# Parse command line arguments
case "${1:-run}" in
    compile)
        compile_firmware
        ;;
    reset)
        reset_device
        ;;
    serial)
        read_serial
        ;;
    analyze)
        LOG_FILE="${LOG_DIR}/$(ls -t ${LOG_DIR}/test_*.log 2>/dev/null | head -1 | xargs basename)"
        if [ -f "$LOG_DIR/$LOG_FILE" ]; then
            analyze_startup
        else
            log ERROR "No log file found to analyze"
            exit 1
        fi
        ;;
    run)
        check_dependencies
        setup_serial
        main
        ;;
    *)
        echo "Usage: $0 {run|compile|reset|serial|analyze}"
        exit 1
        ;;
esac