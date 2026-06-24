import paramiko
import time
import threading

SERVER = '192.168.49.81'
USER = 'lynxi'
PASS = '1'

def monitor_serial():
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(SERVER, username=USER, password=PASS)
    
    # 清理串口占用
    ssh.exec_command('echo 1 | sudo -S pkill -9 -f ttyUSB0 2>/dev/null')
    time.sleep(2)
    
    # 启动后台串口监控
    stdin, stdout, stderr = ssh.exec_command('cat /dev/ttyUSB0')
    
    time.sleep(1)
    # 复位设备
    ssh.exec_command('echo 1 | sudo -S lynd_hp run -d 0 -r wdt -o5')
    
    # 读取输出 40秒
    output_lines = []
    start = time.time()
    while time.time() - start < 40:
        line = stdout.readline()
        if line:
            output_lines.append(line)
            print(line, end='')  # 实时打印
    
    return ''.join(output_lines)

output = monitor_serial()
print('\n=== Test Complete ===')
