#!/usr/bin/env python3
"""
HP232X 自动化测试脚本
自动处理SSH连接和sudo密码
"""

import paramiko
import time
import sys
import re

# 测试服务器配置
SERVER = "192.168.49.81"
USERNAME = "lynxi"
PASSWORD = "1"
SUDO_PASSWORD = "1"

def run_test():
    """执行完整的测试流程"""

    print("=== HP232X Automated Test ===")
    print()

    # 创建SSH客户端
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        # 连接服务器
        print("[1] Connecting to server...")
        ssh.connect(SERVER, username=USERNAME, password=PASSWORD, timeout=10)
        print("    ✓ Connected")

        # 验证固件文件
        print("\n[2] Verifying firmware file...")
        stdin, stdout, stderr = ssh.exec_command(
            "ls -lh /mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin"
        )
        firmware_info = stdout.read().decode().strip()
        print(f"    Firmware: {firmware_info}")

        # 验证软链接
        stdin, stdout, stderr = ssh.exec_command(
            "ls -l /lib/firmware/lyn_drv/boot-wrapper.bin"
        )
        link_info = stdout.read().decode().strip()
        print(f"    Symlink: {link_info}")

        # 清理串口占用
        print("\n[3] Cleaning serial port...")
        stdin, stdout, stderr = ssh.exec_command(
            f"echo '{SUDO_PASSWORD}' | sudo -S pkill -9 -f 'ttyUSB' 2>/dev/null || true"
        )
        time.sleep(2)

        # 设置串口波特率
        stdin, stdout, stderr = ssh.exec_command(
            f"echo '{SUDO_PASSWORD}' | sudo -S stty -F /dev/ttyUSB1 115200 raw -echo"
        )
        time.sleep(1)

        # 启动串口监控（后台）
        print("[4] Starting serial monitor (20s timeout)...")
        # 使用pexpect来监控串口
        transport = ssh.get_transport()
        session = transport.open_session()
        session.get_pty()
        session.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S timeout 20 cat /dev/ttyUSB1")

        # 等待监控启动
        time.sleep(2)

        # 执行设备复位
        print("[5] Resetting device...")
        stdin, stdout, stderr = ssh.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S lynd_hp run -d 0 -r wdt -o5")
        reset_exit_status = stdout.channel.recv_exit_status()
        if reset_exit_status == 0:
            print("    ✓ Reset command executed")
        else:
            print(f"    ⚠ Reset command exited with status: {reset_exit_status}")

        # 读取串口输出
        print("\n[6] Capturing serial output...")
        full_output = ""
        start_time = time.time()
        timeout = 25  # 20秒监控 + 5秒缓冲

        while time.time() - start_time < timeout:
            try:
                if session.recv_ready():
                    chunk = session.recv(8192).decode(errors='ignore')
                    if chunk:
                        full_output += chunk
                        print(chunk, end='')
                        sys.stdout.flush()
                else:
                    time.sleep(0.05)
            except Exception as e:
                print(f"\n    Error reading serial: {e}")
                break

        session.close()

        print("\n\n[7] Analysis Results:")
        print("-" * 50)

        # 检测关键标记
        print("  [Boot Process]")
        if "STEP1" in full_output:
            print("    ✓ rt_page_init reached")
        else:
            print("    ✗ rt_page_init NOT reached")

        if "STEP2" in full_output:
            print("    ✓ rt_page_init completed")
        else:
            print("    ✗ rt_page_init failed or hung")

        if "STEP3" in full_output:
            print("    ✓ rt_hw_mmu_setup reached")
        else:
            print("    ✗ rt_hw_mmu_setup NOT reached")

        if "STEP4" in full_output:
            print("    ✓ rt_hw_mmu_setup completed")
        else:
            print("    ✗ rt_hw_mmu_setup failed or hung")

        print("\n  [System Status]")
        if "msh" in full_output or "shell" in full_output.lower():
            print("    ✓ Shell detected")
        else:
            print("    ✗ Shell NOT detected")

        if "PC:0x401fff8" in full_output:
            print("    ⚠ PC异常 @ doorbell地址 - 栈溢出或返回地址损坏")
        elif "EXC" in full_output:
            print("    ⚠ 检测到异常中断/复位")

        if "ERROR" in full_output or "panic" in full_output:
            print("    ⚠ 错误模式检测到")
            error_lines = [line for line in full_output.split('\n') if 'ERROR' in line or 'panic' in line]
            for line in error_lines[:3]:
                print(f"       {line.strip()}")
        else:
            print("    ✓ 无明显错误")

        if "STEP1" in full_output:
            print("    ✓ rt_page_init reached")
        else:
            print("    ✗ rt_page_init NOT reached")

        if "STEP2" in full_output:
            print("    ✓ rt_page_init completed")
        else:
            print("    ✗ rt_page_init failed or hung")

        if "STEP3" in full_output:
            print("    ✓ rt_hw_mmu_setup reached")
        else:
            print("    ✗ rt_hw_mmu_setup NOT reached")

        if "STEP4" in full_output:
            print("    ✓ rt_hw_mmu_setup completed")
        else:
            print("    ✗ rt_hw_mmu_setup failed or hung")

        if "msh" in full_output or "shell" in full_output.lower():
            print("    ✓ Shell detected")
        else:
            print("    ✗ Shell NOT detected")

        if "ERROR" in full_output or "panic" in full_output:
            print("    ⚠ Errors detected")
        else:
            print("    ✓ No errors detected")

        print("-" * 50)
        print("\n=== Test Complete ===")
        return True

    except Exception as e:
        print(f"\n✗ Test failed with error: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        ssh.close()

if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)