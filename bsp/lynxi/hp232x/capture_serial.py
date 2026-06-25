#!/usr/bin/env python3
"""
简单串口输出捕获脚本
"""

import paramiko
import time
import sys

SERVER = "192.168.49.81"
USERNAME = "lynxi"
PASSWORD = "1"
SUDO_PASSWORD = "1"

def capture_serial():
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        ssh.connect(SERVER, username=USERNAME, password=PASSWORD, timeout=10)

        # 清理串口
        ssh.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S pkill -9 -f 'ttyUSB1' 2>/dev/null || true")
        time.sleep(1)

        # 设置串口
        ssh.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S stty -F /dev/ttyUSB1 115200 raw -echo")
        time.sleep(1)

        # 启动串口捕获
        transport = ssh.get_transport()
        session = transport.open_session()
        session.get_pty()
        session.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S timeout 30 cat /dev/ttyUSB1")

        time.sleep(2)

        # 复位设备
        ssh.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S lynd_hp run -d 0 -r wdt -o5")
        print("Device reset...")

        # 等待30秒
        time.sleep(30)

        # 读取串口输出
        output = ""
        while session.recv_ready():
            chunk = session.recv(4096).decode('utf-8', errors='ignore')
            output += chunk
            print(chunk, end='', flush=True)

        # 保存到文件
        with open('serial_output.txt', 'w') as f:
            f.write(output)

        print("\n\n=== Output saved to serial_output.txt ===")

    except Exception as e:
        print(f"Error: {e}")
    finally:
        ssh.close()

if __name__ == "__main__":
    capture_serial()