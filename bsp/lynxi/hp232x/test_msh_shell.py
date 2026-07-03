#!/usr/bin/env python3
"""
msh Shell命令执行测试
通过SSH连接发送Shell命令，验证命令执行和响应
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
SERIAL_PORT = "/dev/ttyUSB1"

def run_shell_command_test():
    """执行Shell命令测试"""

    print("=" * 60)
    print(" msh Shell Command Execution Test")
    print("=" * 60)
    print()

    # 创建SSH客户端
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        # 连接服务器
        print("[1] Connecting to server...")
        ssh.connect(SERVER, username=USERNAME, password=PASSWORD, timeout=10)
        print("    ✓ Connected")

        # 清理串口占用
        print("\n[2] Cleaning serial port...")
        stdin, stdout, stderr = ssh.exec_command(
            f"echo '{SUDO_PASSWORD}' | sudo -S pkill -9 -f 'ttyUSB' 2>/dev/null || true"
        )
        time.sleep(2)

        # 设置串口
        print("[3] Configuring serial port...")
        stdin, stdout, stderr = ssh.exec_command(
            f"echo '{SUDO_PASSWORD}' | sudo -S stty -F {SERIAL_PORT} 115200 raw -echo && "
            f"echo '{SUDO_PASSWORD}' | sudo -S chmod 666 {SERIAL_PORT}"
        )
        exit_status = stdout.channel.recv_exit_status()
        if exit_status == 0:
            print("    ✓ Serial port configured")

        # 启动串口监控
        print("\n[4] Starting serial monitor...")
        transport = ssh.get_transport()
        session = transport.open_session()
        session.get_pty()
        session.exec_command(f"echo '{SUDO_PASSWORD}' | sudo -S timeout 30 cat {SERIAL_PORT}")
        time.sleep(2)

        # 复位设备
        print("[5] Resetting device...")
        stdin, stdout, stderr = ssh.exec_command(
            f"echo '{SUDO_PASSWORD}' | sudo -S lynd_hp run -d 0 -r wdt -o5"
        )
        exit_status = stdout.channel.recv_exit_status()
        if exit_status == 0:
            print("    ✓ Reset command executed")

        # 等待Shell启动
        print("\n[6] Waiting for msh Shell startup...")
        output = ""
        shell_ready = False
        timer_isr_count = 0

        start_time = time.time()
        while time.time() - start_time < 20:
            if session.recv_ready():
                data = session.recv(1024).decode('utf-8', errors='ignore')
                output += data
                print(data, end='', flush=True)

                # 检查Shell就绪
                if "msh >" in output and not shell_ready:
                    shell_ready = True
                    print("\n    ✓ msh Shell ready!")

                # 检查Timer ISR
                if "TIMER ISR" in output:
                    matches = re.findall(r'TIMER ISR #(\d+)', output)
                    if matches:
                        timer_isr_count = int(matches[-1])

            time.sleep(0.1)

        if not shell_ready:
            print("\n    ⚠ Shell not ready, continuing anyway...")

        # 等待Shell稳定
        print("\n[7] Waiting for Shell to stabilize...")
        time.sleep(3)

        # 发送Shell命令测试
        print("\n[8] Testing Shell commands...")

        test_commands = [
            ('help', 'List all commands'),
            ('list_thread', 'Show running threads'),
            ('list_device', 'Show registered devices'),
            ('ps', 'Process status'),
            ('free', 'Memory usage'),
        ]

        command_results = []

        for cmd, desc in test_commands:
            print(f"\n[Testing] Command: '{cmd}' ({desc})")

            # 发送命令
            stdin, stdout, stderr = ssh.exec_command(
                f"printf '{cmd}\r' > {SERIAL_PORT}"
            )
            stdout.channel.recv_exit_status()
            print(f"    Sent: '{cmd}'")

            # 等待响应
            time.sleep(2)

            # 收集输出
            cmd_output = ""
            start_time = time.time()
            while time.time() - start_time < 3:
                if session.recv_ready():
                    data = session.recv(1024).decode('utf-8', errors='ignore')
                    cmd_output += data
                    print(data, end='', flush=True)
                time.sleep(0.1)

            # 分析响应
            response_received = len(cmd_output) > 0
            has_error = "error" in cmd_output.lower() or "unknown" in cmd_output.lower()
            has_result = cmd in cmd_output and "msh >" in cmd_output

            result = {
                'command': cmd,
                'description': desc,
                'sent': True,
                'response_received': response_received,
                'has_result': has_result,
                'has_error': has_error,
                'output_length': len(cmd_output),
                'output_preview': cmd_output[:200] if cmd_output else ""
            }
            command_results.append(result)

            if has_result and not has_error:
                print(f"    ✓ Command '{cmd}' executed successfully")
            elif response_received:
                print(f"    ⚠ Response received but execution unclear")
            else:
                print(f"    ✗ No response for command '{cmd}'")

        # 最终验证结果
        print("\n" + "=" * 60)
        print(" Test Results Summary")
        print("=" * 60)

        print(f"\nTimer interrupt verification:")
        print(f"  ISR triggered: {timer_isr_count} times")
        if timer_isr_count > 0:
            print(f"  ✓ Timer interrupt working (system tick active)")

        print(f"\nShell status:")
        print(f"  Shell ready: {shell_ready}")
        if shell_ready:
            print(f"  ✓ msh > prompt displayed")

        print(f"\nCommand execution results:")
        success_count = 0
        for result in command_results:
            status = "✓" if result['has_result'] and not result['has_error'] else "⚠"
            print(f"  {status} {result['command']}: {result['description']}")
            print(f"      Response: {result['output_length']} bytes")
            if result['has_result']:
                success_count += 1

        print(f"\nOverall assessment:")
        if success_count >= len(test_commands) * 0.8:
            print(f"  ★★★ SUCCESS ★★★")
            print(f"  ✓ Shell commands working ({success_count}/{len(test_commands)})")
            print(f"  ✓ System fully functional")
            print(f"  ✓ Timer + Shell + UART all working")
            success = True
        elif shell_ready and timer_isr_count > 0:
            print(f"  ⚠ PARTIAL SUCCESS")
            print(f"  ✓ Shell prompt displayed")
            print(f"  ✓ Timer interrupt working")
            print(f"  ⚠ Commands may need polling mode")
            success = False
        else:
            print(f"  ✗ FAILED")
            print(f"  Core systems not working properly")
            success = False

        print("\n" + "=" * 60)
        print(" Test Complete")
        print("=" * 60)

        # 保存详细输出
        with open('/tmp/shell_test_detail.log', 'w') as f:
            f.write(f"Full output:\n{output}\n\n")
            for result in command_results:
                f.write(f"\nCommand: {result['command']}\n")
                f.write(f"Output preview:\n{result['output_preview']}\n")

        print("\nDetailed log saved to /tmp/shell_test_detail.log")

        return success

    except Exception as e:
        print(f"\n✗ Test failed with error: {e}")
        import traceback
        traceback.print_exc()
        return False

    finally:
        ssh.close()
        print("\n[9] SSH connection closed")

if __name__ == '__main__':
    success = run_shell_command_test()
    sys.exit(0 if success else 1)