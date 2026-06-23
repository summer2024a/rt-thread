#!/usr/bin/env python3
"""
HP232X Automated Testing Framework
Handles compilation, firmware update, and serial monitoring
"""

import os
import sys
import time
import threading
import subprocess
import paramiko
import serial
from datetime import datetime
import re
import argparse

# Configuration
CONFIG = {
    'test_server': '192.168.49.81',
    'server_user': 'lynxi',
    'server_pass': '1',
    'serial_port': '/dev/ttyUSB0',
    'baud_rate': 115200,
    'local_fw_path': '/work/rt-thread/bsp/lynxi/hp232x',
    'upgrade_cmd': 'lynd_hp run -d 0 -r wdt -o5',
    'max_iterations': 10,
    'serial_timeout': 60,
}

# ANSI Colors
COLORS = {
    'RED': '\033[0;31m',
    'GREEN': '\033[0;32m',
    'YELLOW': '\033[1;33m',
    'BLUE': '\033[0;34m',
    'NC': '\033[0m',
}

class Logger:
    """Logging utility with file and console output"""
    def __init__(self, log_dir='./test_logs'):
        os.makedirs(log_dir, exist_ok=True)
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        self.log_file = os.path.join(log_dir, f'test_{timestamp}.log')
        self.serial_file = os.path.join(log_dir, f'serial_{timestamp}.log')
    
    def log(self, level, message):
        """Log message to console and file"""
        color = COLORS.get(level, '')
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        formatted = f'{color}[{timestamp}] [{level}] {message}{COLORS["NC"]}'
        print(formatted)
        
        with open(self.log_file, 'a') as f:
            f.write(f'[{timestamp}] [{level}] {message}\n')
    
    def info(self, message): self.log('INFO', message)
    def success(self, message): self.log('SUCCESS', message)
    def warning(self, message): self.log('WARNING', message)
    def error(self, message): self.log('ERROR', message)

class SerialMonitor:
    """Serial port monitoring with pattern detection"""
    def __init__(self, logger):
        self.logger = logger
        self.stop_event = threading.Event()
        self.serial_text = []
        self.mshell_detected = False
        
    def monitor(self):
        """Monitor serial port and detect patterns"""
        try:
            with serial.Serial(
                CONFIG['serial_port'],
                CONFIG['baud_rate'],
                timeout=0.1
            ) as ser:
                self.logger.info(f'Monitoring serial port {CONFIG["serial_port"]}')
                
                # Clear any existing data
                ser.reset_input_buffer()
                
                start_time = time.time()
                char_sequence = []
                
                while not self.stop_event.is_set() and \
                      (time.time() - start_time) < CONFIG['serial_timeout']:
                    if ser.in_waiting > 0:
                        data = ser.read(ser.in_waiting)
                        try:
                            text = data.decode('utf-8', errors='ignore')
                            self.serial_text.append(text)
                            
                            # Extract startup characters (P, E, X, .)
                            for char in text:
                                if char in ['P', 'E', 'X', '.']:
                                    char_sequence.append(char)
                            
                            # Check for MSH shell
                            if 'msh' in text or 'msh>' in text:
                                self.logger.success('✓✓✓ MSH SHELL DETECTED ✓✓✓')
                                self.mshell_detected = True
                                self.stop_event.set()
                                break
                            
                            # Real-time output
                            print(text, end='', flush=True)
                            
                            # Write to serial log
                            with open(self.logger.serial_file, 'a') as f:
                                f.write(text)
                        except:
                            pass
                    
                    time.sleep(0.01)
                
                print()  # New line after monitoring
                
                # Analyze character sequence
                if char_sequence:
                    self.analyze_startup_seq(char_sequence)
                
                return self.mShell_detected
                
        except serial.SerialException as e:
            self.logger.error(f'Serial port error: {e}')
            return False
    
    def analyze_startup_seq(self, char_sequence):
        """Analyze startup character sequence"""
        if len(char_sequence) > 200:
            char_sequence = char_sequence[:200]
        
        sequence_str = ''.join(char_sequence)
        self.logger.info(f'Startup character sequence: {sequence_str}')
        
        p_count = char_sequence.count('P')
        e_count = char_sequence.count('E')
        x_count = char_sequence.count('X')
        dot_count = char_sequence.count('.')
        
        self.logger.info('Pattern analysis:')
        self.logger.info(f'  - P (primary CPU): {p_count}')
        self.logger.info(f'  - E (exception level): {e_count}')
        self.logger.info(f'  - X (expected stop): {x_count}')
        self.logger.info(f'  - . (BSS progress): {dot_count} (≈{dot_count * 64} bytes)')
        
        return sequence_str
    
    def stop(self):
        """Stop monitoring"""
        self.stop_event.set()

class RemoteDevice:
    """Remote device control via SSH"""
    def __init__(self, logger):
        self.logger = logger
        self.ssh = None
    
    def connect(self):
        """Connect to remote server"""
        try:
            self.ssh = paramiko.SSHClient()
            self.ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            self.ssh.connect(
                CONFIG['test_server'],
                username=CONFIG['server_user'],
                password=CONFIG['server_pass'],
                timeout=10
            )
            self.logger.success(f'✓ Connected to {CONFIG["test_server"]}')
            return True
        except Exception as e:
            self.logger.error(f'SSH connection failed: {e}')
            return False
    
    def reset_device(self):
        """Reset device to update firmware"""
        try:
            self.logger.info('Resetting device to update firmware...')
            
            # Execute upgrade command with sudo
            channel = self.ssh.invoke_shell()
            time.sleep(0.5)
            channel.send(f'sudo {CONFIG["upgrade_cmd"]}\n')
            time.sleep(0.5)
            channel.send(f'{CONFIG["server_pass"]}\n')
            
            # Wait for command to complete
            time.sleep(3)
            
            self.logger.success('✓ Device reset command executed')
            return True
        except Exception as e:
            self.logger.error(f'Device reset failed: {e}')
            return False
    
    def verify_firmware_link(self):
        """Verify firmware symlink exists"""
        try:
            cmd = "ls -l /lib/firmware/lyn_drv/boot-wrapper.bin"
            stdin, stdout, stderr = self.ssh.exec_command(cmd)
            output = stdout.read().decode()
            
            if 'rtthread-header.bin' in output:
                self.logger.success('✓ Firmware symlink verified')
                self.logger.info(f'  Link: {output.strip()}')
                return True
            else:
                self.logger.warning('⚠ Firmware symlink not pointing to correct location')
                return False
        except Exception as e:
            self.logger.error(f'Failed to verify firmware link: {e}')
            return False
    
    def disconnect(self):
        """Disconnect from remote server"""
        if self.ssh:
            self.ssh.close()
            self.logger.info('Disconnected from remote server')

class Compiler:
    """Firmware compilation"""
    def __init__(self, logger):
        self.logger = logger
    
    def compile(self):
        """Compile RT-Thread firmware"""
        try:
            self.logger.info('Compiling RT-Thread firmware...')
            os.chdir(CONFIG['local_fw_path'])
            
            # Clean and build
            if os.path.exists('build'):
                subprocess.run(['rm', '-rf', 'build'], check=True)
            
            result = subprocess.run(
                ['scons', '-j' + str(os.cpu_count())],
                capture_output=True,
                text=True
            )
            
            if result.returncode == 0:
                self.logger.success('✓ Firmware compiled successfully')
                
                # Check firmware size
                fw_path = 'rtthread-header.bin'
                if os.path.exists(fw_path):
                    size = os.path.getsize(fw_path)
                    self.logger.info(f'Firmware size: {size} bytes')
                    
                    # Verify within target range (200-300KB)
                    if size < 204800 or size > 307200:
                        self.logger.warning(
                            f'⚠ Firmware size {size} bytes outside target range (200-300KB)'
                        )
                    else:
                        self.logger.success('✓ Firmware size within target range')
                    
                    return True
                else:
                    self.logger.error('✗ Firmware binary not found')
                    return False
            else:
                self.logger.error('✗ Firmware compilation failed')
                self.logger.error(result.stderr)
                return False
                
        except Exception as e:
            self.logger.error(f'Compilation error: {e}')
            return False

class TestFramework:
    """Main testing framework"""
    def __init__(self):
        self.logger = Logger()
        self.compiler = None
        self.remote = None
        self.serial_monitor = None
    
    def setup(self):
        """Setup testing environment"""
        self.logger.info('=' * 50)
        self.logger.info('HP232X Automated Testing Framework')
        self.logger.info('=' * 50)
        
        # Initialize components
        self.compiler = Compiler(self.logger)
        self.remote = RemoteDevice(self.logger)
        self.serial_monitor = SerialMonitor(self.logger)
        
        # Check serial port
        if not os.path.exists(CONFIG['serial_port']):
            self.logger.error(f'Serial port not found: {CONFIG["serial_port"]}')
            return False
        
        # Connect to remote server
        if not self.remote.connect():
            return False
        
        # Verify firmware symlink
        self.remote.verify_firmware_link()
        
        return True
    
    def run_test_iteration(self, iteration, max_iterations):
        """Run a single test iteration"""
        self.logger.info('=' * 50)
        self.logger.info(f'Test Iteration {iteration}/{max_iterations}')
        self.logger.info('=' * 50)
        
        # Step 1: Compile firmware
        if not self.compiler.compile():
            self.logger.error('Compilation failed, stopping test loop')
            return False
        
        # Step 2: Reset device
        if not self.remote.reset_device():
            self.logger.warning('Reset failed, but continuing to monitor serial...')
        
        # Step 3: Monitor serial in separate thread
        serial_thread = threading.Thread(target=self.serial_monitor.monitor)
        serial_thread.start()
        
        # Wait for serial monitoring to complete
        serial_thread.join(timeout=CONFIG['serial_timeout'] + 5)
        
        # Stop monitoring if still running
        self.serial_monitor.stop()
        
        # Check result
        if self.serial_monitor.mShell_detected:
            self.logger.success('=' * 50)
            self.logger.success('✓✓✓ MSH SHELL SUCCESSFULLY STARTED ✓✓✓')
            self.logger.success('=' * 50)
            return True
        else:
            self.logger.warning(f'MSH shell not detected in iteration {iteration}')
            time.sleep(2)
            return False
    
    def run(self, max_iterations=None):
        """Run complete test loop"""
        if max_iterations is None:
            max_iterations = CONFIG['max_iterations']
        
        if not self.setup():
            return 1
        
        try:
            for i in range(1, max_iterations + 1):
                if self.run_test_iteration(i, max_iterations):
                    return 0
            
            self.logger.error('=' * 50)
            self.logger.error(f'Testing failed after {max_iterations} iterations')
            self.logger.error('=' * 50)
            return 1
            
        finally:
            self.remote.disconnect()
    
    def stats(self):
        """Display test statistics"""
        log_dir = './test_logs'
        if os.path.exists(log_dir):
            log_files = sorted([f for f in os.listdir(log_dir) 
                              if f.startswith('test_') and f.endswith('.log')])
            
            if log_files:
                self.logger.info(f'Found {len(log_files)} test logs')
                latest_log = log_files[-1]
                self.logger.info(f'Latest log: {latest_log}')

def main():
    parser = argparse.ArgumentParser(description='HP232X Automated Testing Framework')
    parser.add_argument('command', 
                       choices=['run', 'compile', 'serial', 'stats'],
                       help='Command to execute')
    parser.add_argument('--iterations', '-i', type=int,
                       help='Number of test iterations')
    
    args = parser.parse_args()
    
    framework = TestFramework()
    
    if args.command == 'run':
        sys.exit(framework.run(args.iterations))
    elif args.command == 'compile':
        framework.setup()  # Just initialize logger
        sys.exit(0 if framework.compiler.compile() else 1)
    elif args.command == 'serial':
        if not os.path.exists(CONFIG['serial_port']):
            print(f'Error: Serial port not found: {CONFIG["serial_port"]}', file=sys.stderr)
            sys.exit(1)
        
        framework.setup()  # Just initialize logger
        framework.serial_monitor = SerialMonitor(framework.logger)
        result = framework.serial_monitor.monitor()
        sys.exit(0 if result else 1)
    elif args.command == 'stats':
        framework.stats()

if __name__ == '__main__':
    main()