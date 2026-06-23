#!/usr/bin/env python3
"""
HP232X 自动化测试脚本 / Automated testing script for HP232X

功能:
1. 编译 RT-Thread 固件 / Compile RT-Thread firmware
2. 通过 SSH 连接到测试服务器 / Connect to test server via SSH
3. 监控串口输出 / Monitor serial port output
4. 执行固件升级和复位 / Execute firmware upgrade and reset
5. 分析启动结果 / Analyze boot results

使用方法 / Usage:
    python3 test_hp232x.py [options]

环境要求 / Requirements:
    - Python 3.6+
    - paramiko (SSH): pip install paramiko
    - pyserial: pip install pyserial
    - SSH access to 192.168.49.81
"""

import paramiko
import serial
import threading
import time
import subprocess
import sys
import os
import re
from pathlib import Path
from datetime import datetime

# ==================== 配置 / Configuration ====================

SERVER_CONFIG = {
    'host': '192.168.49.81',
    'username': 'lynxi',
    'password': '1',
    'port': 22
}

SERIAL_CONFIG = {
    'port': '/dev/ttyUSB0',
    'baudrate': 115200,
    'timeout': 1
}

LOCAL_PATHS = {
    'rtthread_root': '/work/rt-thread',
    'bsp_dir': '/work/rt-thread/bsp/lynxi/hp232x',
    'firmware': '/work/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin'
}

REMOTE_PATHS = {
    'rtthread_mount': '/mnt/49.20/rt-thread',
    'firmware_link': '/lib/firmware/lyn_drv/boot-wrapper.bin',
    'bsp_dir': '/mnt/49.20/rt-thread/bsp/lynxi/hp232x'
}

COMMANDS = {
    'reset': 'sudo lynd_hp run -d 0 -r wdt -o5',
    'check_link': 'ls -l /lib/firmware/lyn_drv/boot-wrapper.bin',
    'check_file': 'ls -lh /mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin',
}

# 启动模式标记 / Boot mode markers
BOOT_MARKERS = {
    'shell_prompt': r'msh />',
    'heap_info': r'Memory.*heap:\s+(\d+)\s+KB',
    'gic_init': r'GIC.*init',
    'smp_start': r'cpu\[\d+\]: running',
    'error_patterns': [
        r'ERROR',
        r'error',
        r'Exception',
        r'exception',
        r'PANIC',
        r'panic',
        r'Fault',
        r'fault',
    ]
}

# ==================== 日志类 / Logging ====================

class TestLogger:
    """测试日志管理器 / Test logger manager"""

    def __init__(self, log_dir='logs'):
        self.log_dir = Path(log_dir)
        self.log_dir.mkdir(exist_ok=True)
        self.session_id = datetime.now().strftime('%Y%m%d_%H%M%S')
        self.log_file = self.log_dir / f'test_{self.session_id}.log'
        self.serial_file = self.log_dir / f'serial_{self.session_id}.log'
        self.start_time = time.time()

    def log(self, level, message):
        """记录日志 / Log message"""
        timestamp = datetime.now().strftime('%H:%M:%S')
        log_line = f'[{timestamp}] [{level}] {message}'
        print(log_line)
        with open(self.log_file, 'a', encoding='utf-8') as f:
            f.write(log_line + '\n')

    def info(self, message):
        self.log('INFO', message)

    def warning(self, message):
        self.log('WARN', message)

    def error(self, message):
        self.log('ERROR', message)

    def success(self, message):
        self.log('SUCCESS', message)

    def serial_log(self, line):
        """记录串口数据 / Log serial data"""
        with open(self.serial_file, 'a', encoding='utf-8', errors='ignore') as f:
            f.write(line + '\n')

    def save_serial_output(self, output):
        """保存完整串口输出 / Save complete serial output"""
        output_file = self.log_dir / f'boot_output_{self.session_id}.txt'
        with open(output_file, 'w', encoding='utf-8', errors='ignore') as f:
            f.write(output)
        self.info(f'串口输出已保存到: {output_file}')
        return output_file

# ==================== 编译类 / Build ====================

class HP232XBuilder:
    """HP232X 固件编译器 / HP232X firmware builder"""

    def __init__(self, bsp_dir, logger):
        self.bsp_dir = Path(bsp_dir)
        self.logger = logger

    def clean(self):
        """清理构建目录 / Clean build directory"""
        self.logger.info(f'清理构建目录: {self.bsp_dir}')
        result = subprocess.run(
            ['rm', '-rf', 'build'],
            cwd=self.bsp_dir,
            capture_output=True,
            text=True
        )
        if result.returncode != 0:
            self.logger.warning(f'清理构建目录失败 (可能不存在): {result.stderr}')
        return True

    def build(self, parallel_jobs=None):
        """编译固件 / Build firmware"""
        if parallel_jobs is None:
            parallel_jobs = str(os.cpu_count())

        self.logger.info(f'开始编译 (并行任务数: {parallel_jobs})')
        start_time = time.time()

        result = subprocess.run(
            ['scons', '-j', parallel_jobs],
            cwd=self.bsp_dir,
            capture_output=True,
            text=True
        )

        duration = time.time() - start_time

        if result.returncode == 0:
            self.logger.success(f'编译成功 (耗时: {duration:.1f}s)')
            return True, result.stdout
        else:
            self.logger.error(f'编译失败 (耗时: {duration:.1f}s)')
            self.logger.error(f'错误输出:\n{result.stderr}')
            return False, result.stderr

    def check_binary_size(self):
        """检查二进制文件大小 / Check binary size"""
        firmware_path = self.bsp_dir / 'rtthread-header.bin'
        if not firmware_path.exists():
            self.logger.error(f'固件文件不存在: {firmware_path}')
            return False, 0

        size = firmware_path.stat().st_size
        size_kb = size / 1024

        self.logger.info(f'固件大小: {size_kb:.2f} KB ({size} bytes)')

        # 检查目标范围 / Check target range
        if 200 <= size_kb <= 300:
            self.logger.success(f'固件大小在目标范围内 (200-300KB)')
            return True, size_kb
        elif size_kb > 300:
            self.logger.warning(f'固件大小超过目标 (200-300KB)')
            return True, size_kb
        else:
            self.logger.warning(f'固件大小小于目标 (200-300KB)')
            return True, size_kb

# ==================== SSH 连接类 / SSH Connection ====================

class SSHClient:
    """SSH 客户端管理器 / SSH client manager for test server"""

    def __init__(self, config, logger):
        self.config = config
        self.logger = logger
        self.client = None

    def connect(self):
        """连接到 SSH 服务器 / Connect to SSH server"""
        self.logger.info(f'连接到 SSH 服务器: {self.config["host"]}')
        try:
            self.client = paramiko.SSHClient()
            self.client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            self.client.connect(
                self.config['host'],
                port=self.config['port'],
                username=self.config['username'],
                password=self.config['password'],
                timeout=10
            )
            self.logger.success('SSH 连接成功')
            return True
        except Exception as e:
            self.logger.error(f'SSH 连接失败: {e}')
            return False

    def execute(self, command, timeout=30):
        """执行命令 / Execute command"""
        self.logger.info(f'执行命令: {command}')
        try:
            stdin, stdout, stderr = self.client.exec_command(command, timeout=timeout)
            exit_status = stdout.channel.recv_exit_status()
            output = stdout.read().decode('utf-8', errors='ignore')
            error = stderr.read().decode('utf-8', errors='ignore')

            if exit_status != 0:
                self.logger.warning(f'命令退出状态: {exit_status}')
                if error:
                    self.logger.warning(f'错误输出: {error.strip()}')

            return exit_status, output, error
        except Exception as e:
            self.logger.error(f'执行命令失败: {e}')
            return -1, '', str(e)

    def check_firmware_link(self):
        """检查固件软链接 / Check firmware symlink"""
        self.logger.info('检查固件软链接')
        exit_status, output, error = self.execute(COMMANDS['check_link'])

        if exit_status == 0:
            if '/mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin' in output:
                self.logger.success('固件软链接正确')
                return True
            else:
                self.logger.warning(f'固件软链接异常:\n{output}')
                return False
        else:
            self.logger.error(f'无法检查固件软链接: {error}')
            return False

    def check_firmware_file(self):
        """检查固件文件是否存在 / Check if firmware file exists"""
        self.logger.info('检查固件文件')
        exit_status, output, error = self.execute(COMMANDS['check_file'])

        if exit_status == 0 and output.strip():
            self.logger.success(f'固件文件已挂载:\n{output.strip()}')
            return True
        else:
            self.logger.error(f'固件文件未挂载或不存在')
            return False

    def reset_device(self):
        """复位设备 / Reset device"""
        self.logger.info('复位设备...')
        exit_status, output, error = self.execute(COMMANDS['reset'], timeout=60)

        if exit_status == 0:
            self.logger.success('设备复位成功')
            return True
        else:
            self.logger.error(f'设备复位失败: {error}')
            return False

    def close(self):
        """关闭连接 / Close connection"""
        if self.client:
            self.client.close()
            self.logger.info('SSH 连接已关闭')

# ==================== 串口监控类 / Serial Monitor ====================

class SerialMonitor:
    """串口监控器 / Serial monitor"""

    def __init__(self, config, logger):
        self.config = config
        self.logger = logger
        self.serial_conn = None
        self.running = False
        self.output = []
        self.boot_complete = threading.Event()
        self.boot_success = False
        self.boot_info = {}

    def connect(self):
        """连接串口 / Connect to serial port"""
        self.logger.info(f'连接串口: {self.config["port"]} @ {self.config["baudrate"]} baud')
        try:
            self.serial_conn = serial.Serial(
                self.config['port'],
                self.config['baudrate'],
                timeout=self.config['timeout']
            )
            if self.serial_conn.is_open:
                self.logger.success('串口连接成功')
                # 清空缓冲区 / Clear buffer
                self.serial_conn.reset_input_buffer()
                self.serial_conn.reset_output_buffer()
                return True
            else:
                self.logger.error('串口未打开')
                return False
        except Exception as e:
            self.logger.error(f'串口连接失败: {e}')
            return False

    def start_monitoring(self, timeout=60):
        """开始监控 / Start monitoring"""
        self.logger.info(f'开始监控串口输出 (超时: {timeout}s)')
        self.running = True
        self.output = []

        monitor_thread = threading.Thread(
            target=self._monitor_loop,
            args=(timeout,),
            daemon=True
        )
        monitor_thread.start()

        # 等待启动完成或超时 / Wait for boot complete or timeout
        if self.boot_complete.wait(timeout=timeout + 10):
            self.logger.info('监控完成')
            return True
        else:
            self.logger.warning('监控超时')
            return False

    def _monitor_loop(self, timeout):
        """监控循环 / Monitoring loop"""
        start_time = time.time()
        char_buffer = ''

        while self.running and (time.time() - start_time) < timeout:
            try:
                if self.serial_conn.in_waiting > 0:
                    # 读取一个字符 / Read one character
                    char = self.serial_conn.read(1).decode('utf-8', errors='ignore')
                    char_buffer += char

                    # 检测行结束 / Detect line end
                    if char in ['\n', '\r']:
                        line = char_buffer.strip()
                        if line:
                            self._process_line(line)
                            char_buffer = ''
                else:
                    time.sleep(0.01)
            except Exception as e:
                self.logger.error(f'串口读取错误: {e}')
                break

        self.running = False
        if not self.boot_complete.is_set():
            self.logger.warning(f'监控超时，未检测到 shell 提示')
            self.boot_complete.set()

    def _process_line(self, line):
        """处理一行输出 / Process one line of output"""
        self.logger.serial_log(line)
        self.output.append(line)

        # 打印关键信息 / Print key information
        if any(keyword in line.lower() for keyword in ['error', 'panic', 'exception', 'fault']):
            self.logger.warning(f'检测到错误: {line[:100]}')

        # 检测 shell 提示 / Check shell prompt
        if re.search(BOOT_MARKERS['shell_prompt'], line):
            self.boot_success = True
            self.logger.success('检测到 shell 提示，启动成功!')
            self.boot_complete.set()

        # 检测多核启动 / Check multi-core boot
        smp_match = re.search(r'cpu\[(\d+)\]: running', line)
        if smp_match:
            cpu_id = smp_match.group(1)
            self.logger.info(f'检测到 CPU {cpu_id} 启动')

    def get_output(self):
        """获取完整输出 / Get complete output"""
        return '\n'.join(self.output)

    def analyze_output(self):
        """分析启动输出 / Analyze boot output"""
        output = self.get_output()
        self.logger.info('='*60)
        self.logger.info('启动输出分析 / Boot Output Analysis')
        self.logger.info('='*60)

        analysis = {
            'total_lines': len(self.output),
            'boot_success': self.boot_success,
            'detected_features': [],
            'errors': [],
            'warnings': []
        }

        # 检测错误 / Detect errors
        for pattern in BOOT_MARKERS['error_patterns']:
            if re.search(pattern, output, re.IGNORECASE):
                analysis['errors'].append(f'检测到错误模式: {pattern}')

        # 检测特性 / Detect features
        if re.search(BOOT_MARKERS['shell_prompt'], output):
            analysis['detected_features'].append('MSH Shell 启动成功')

        heap_match = re.search(BOOT_MARKERS['heap_info'], output)
        if heap_match:
            heap_size = heap_match.group(1)
            analysis['detected_features'].append(f'初始化进程堆: {heap_size} KB')

        if re.search(BOOT_MARKERS['gic_init'], output, re.IGNORECASE):
            analysis['detected_features'].append('GIC 中断控制器初始化')

        # 检测多核 / Detect multi-core
        scpus = re.findall(r'cpu\[(\d+)\]: running', output)
        if scpus:
            analysis['detected_features'].append(f'多核启动: {len(set(scpus))} 个 CPU')

        # 统计输出 / Output summary
        self.logger.info(f'总行数: {analysis["total_lines"]}')
        self.logger.info(f'启动状态: {"成功" if analysis["boot_success"] else "失败"}')

        if analysis['detected_features']:
            self.logger.info('检测到的特性:')
            for feature in analysis['detected_features']:
                self.logger.info(f'  ✓ {feature}')

        if analysis['errors']:
            self.logger.warning('检测到的错误:')
            for error in analysis['errors']:
                self.logger.warning(f'  ✗ {error}')

        self.logger.info('='*60)

        return analysis

    def close(self):
        """关闭串口 / Close serial port"""
        self.running = False
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
            self.logger.info('串口连接已关闭')

# ==================== 主测试类 / Main Test Class ====================

class HP232XTestRunner:
    """HP232X 测试运行器 / HP232X test runner"""

    def __init__(self):
        self.logger = TestLogger()
        self.builder = None
        self.ssh_client = None
        self.serial_monitor = None
        self.test_results = []

    def run_test(self, clean_build=True):
        """运行完整测试 / Run complete test"""
        self.logger.info('='*60)
        self.logger.info('开始 HP232X 测试')
        self.logger.info('='*60)

        result = {
            'session_id': self.logger.session_id,
            'timestamp': datetime.now().isoformat(),
            'build': {'success': False, 'size': 0},
            'ssh': {'connected': False, 'firmware_ok': False},
            'boot': {'success': False, 'output_lines': 0}
        }

        # 步骤 1: 编译 / Step 1: Build
        self.logger.info('\n[步骤 1/5] 编译固件')
        self.builder = HP232XBuilder(LOCAL_PATHS['bsp_dir'], self.logger)

        if clean_build:
            self.builder.clean()

        build_ok, build_output = self.builder.build()
        result['build']['success'] = build_ok

        if build_ok:
            size_ok, size_kb = self.builder.check_binary_size()
            result['build']['size'] = size_kb
            if not size_ok:
                self.logger.warning('固件大小超出预期范围')
        else:
            self.logger.error('编译失败，终止测试')
            return result

        # 步骤 2: SSH 连接 / Step 2: SSH connection
        self.logger.info('\n[步骤 2/5] 连接测试服务器')
        self.ssh_client = SSHClient(SERVER_CONFIG, self.logger)

        if not self.ssh_client.connect():
            self.logger.error('SSH 连接失败，终止测试')
            return result
        else:
            result['ssh']['connected'] = True

        # 步骤 3: 检查固件 / Step 3: Check firmware
        self.logger.info('\n[步骤 3/5] 检查固件链接和文件')
        link_ok = self.ssh_client.check_firmware_link()
        file_ok = self.ssh_client.check_firmware_file()
        result['ssh']['firmware_ok'] = link_ok and file_ok

        if not (link_ok and file_ok):
            self.logger.error('固件检查失败，终止测试')
            return result

        # 步骤 4: 串口监控 / Step 4: Serial monitoring
        self.logger.info('\n[步骤 4/5] 启动串口监控')
        self.serial_monitor = SerialMonitor(SERIAL_CONFIG, self.logger)

        if not self.serial_monitor.connect():
            self.logger.error('串口连接失败，终止测试')
            return result

        # 步骤 5: 复位并监控 / Step 5: Reset and monitor
        self.logger.info('\n[步骤 5/5] 复位设备并监控启动')

        # 在后台启动监控 / Start monitoring in background
        monitor_thread = threading.Thread(
            target=self.serial_monitor.start_monitoring,
            args=(60,),
            daemon=True
        )
        monitor_thread.start()

        # 等待串口监控启动 / Wait for serial monitor to start
        time.sleep(2)

        # 执行复位 / Execute reset
        reset_ok = self.ssh_client.reset_device()

        if reset_ok:
            # 等待监控完成 / Wait for monitoring to complete
            monitor_thread.join(timeout=70)

            # 分析输出 / Analyze output
            self.logger.info('\n分析启动输出')
            analysis = self.serial_monitor.analyze_output()

            result['boot']['success'] = analysis['boot_success']
            result['boot']['output_lines'] = analysis['total_lines']
            result['boot']['features'] = analysis['detected_features']
            result['boot']['errors'] = analysis['errors']

            # 保存输出 / Save output
            output_file = self.serial_monitor.get_output()
            self.logger.save_serial_output(output_file)
        else:
            self.logger.error('设备复位失败')

        # 清理 / Cleanup
        self._cleanup()

        # 测试总结 / Test summary
        self._print_summary(result)

        return result

    def _cleanup(self):
        """清理资源 / Cleanup resources"""
        if self.serial_monitor:
            self.serial_monitor.close()
        if self.ssh_client:
            self.ssh_client.close()

    def _print_summary(self, result):
        """打印测试总结 / Print test summary"""
        self.logger.info('='*60)
        self.logger.info('测试总结 / Test Summary')
        self.logger.info('='*60)
        self.logger.info(f'会话 ID: {result["session_id"]}')
        self.logger.info(f'时间戳: {result["timestamp"]}')
        self.logger.info(f'编译: {"成功" if result["build"]["success"] else "失败"}')
        self.logger.info(f'固件大小: {result["build"]["size"]:.2f} KB')
        self.logger.info(f'SSH 连接: {"成功" if result["ssh"]["connected"] else "失败"}')
        self.logger.info(f'固件检查: {"通过" if result["ssh"]["firmware_ok"] else "失败"}')
        self.logger.info(f'启动状态: {"成功" if result["boot"]["success"] else "失败"}')
        self.logger.info(f'输出行数: {result["boot"]["output_lines"]}')

        if 'features' in result['boot'] and result['boot']['features']:
            self.logger.info('检测到的特性: / Detected Features:')
            for feature in result['boot']['features']:
                self.logger.info(f'  ✓ {feature}')

        if 'errors' in result['boot'] and result['boot']['errors']:
            self.logger.warning('检测到的错误: / Detected Errors:')
            for error in result['boot']['errors']:
                self.logger.warning(f'  ✗ {error}')

        self.logger.info('='*60)

        # 保存测试结果 / Save test result
        self.test_results.append(result)
        result_file = self.logger.log_dir / f'result_{result["session_id"]}.json'
        import json
        with open(result_file, 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=2, ensure_ascii=False)
        self.logger.info(f'测试结果已保存: {result_file}')

def main():
    """主函数 / Main function"""
    import argparse

    parser = argparse.ArgumentParser(description='HP232X 自动化测试')
    parser.add_argument(
        '--no-clean',
        action='store_true',
        help='不清理构建目录 / Do not clean build directory'
    )
    parser.add_argument(
        '--log-dir',
        default='logs',
        help='日志目录 / Log directory (default: logs)'
    )
    parser.add_argument(
        '--parallel',
        default=None,
        help='并行编译任务数 / Parallel build jobs (default: CPU count)'
    )

    args = parser.parse_args()

    # 创建测试运行器 / Create test runner
    runner = HP232XTestRunner()

    # 运行测试 / Run test
    result = runner.run_test(clean_build=not args.no_clean)

    # 返回状态码 / Return exit code
    sys.exit(0 if result['boot']['success'] else 1)

if __name__ == '__main__':
    main()