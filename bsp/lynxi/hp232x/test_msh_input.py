#!/usr/bin/env python3
"""Send help to msh over remote serial and check response."""

import paramiko
import sys
import time

SERVER = "192.168.49.81"
USERNAME = "lynxi"
PASSWORD = "1"
SUDO_PASSWORD = "1"

REMOTE_PY = r'''
import time, sys, os, subprocess, select

SUDO = "1"
TTY = "/dev/ttyUSB1"

def sudo(cmd):
    subprocess.run(f"echo '{SUDO}' | sudo -S {cmd}", shell=True, capture_output=True)

sudo("pkill -9 -f ttyUSB1 2>/dev/null || true")
time.sleep(1)
sudo(f"stty -F {TTY} 115200 raw -echo")
time.sleep(0.5)

fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    sudo("lynd_hp run -d 0 -r wdt -o5")
    time.sleep(0.5)
    out = b""
    deadline = time.time() + 25
    saw_msh = False
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            chunk = os.read(fd, 4096)
            if chunk:
                out += chunk
                sys.stdout.buffer.write(chunk)
                sys.stdout.flush()
                if b"msh" in out:
                    saw_msh = True
                    break
    if not saw_msh:
        print("\n[TEST] msh prompt NOT seen", file=sys.stderr)
        sys.exit(2)

    time.sleep(1.0)
    os.write(fd, b"\r\nhelp\r\n")
    print("\n[TEST] Sent: \\r\\nhelp\\r\\n", flush=True)
    time.sleep(3)
    deadline = time.time() + 8
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            chunk = os.read(fd, 4096)
            if chunk:
                out += chunk
                sys.stdout.buffer.write(chunk)
                sys.stdout.flush()

    text = out.decode("utf-8", errors="ignore")
    print("\n[TEST] --- analysis ---")
    if "Data abort" in text or "Execption" in text:
        print("[TEST] FAIL: exception after input")
        sys.exit(3)
    keywords = ["RT-Thread shell commands", "list_thread", "clear", "version", "ps", "command not found"]
    found = [k for k in keywords if k in text]
    print(f"[TEST] help keywords found: {found}")
    if found:
        print("[TEST] PASS: serial input responded")
        sys.exit(0)
    print("[TEST] FAIL: no help response detected")
    sys.exit(4)
finally:
    os.close(fd)
'''


def main():
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(SERVER, username=USERNAME, password=PASSWORD, timeout=10)

    sftp = ssh.open_sftp()
    remote_path = "/tmp/hp232x_msh_test.py"
    with sftp.file(remote_path, "w") as f:
        f.write(REMOTE_PY)
    sftp.close()

    stdin, stdout, stderr = ssh.exec_command(
        f"echo '{SUDO_PASSWORD}' | sudo -S python3 {remote_path}", timeout=90
    )
    while not stdout.channel.exit_status_ready():
        if stdout.channel.recv_ready():
            sys.stdout.write(stdout.channel.recv(4096).decode(errors="ignore"))
            sys.stdout.flush()
        time.sleep(0.05)
    rest = stdout.read().decode(errors="ignore")
    err = stderr.read().decode(errors="ignore")
    code = stdout.channel.recv_exit_status()
    sys.stdout.write(rest)
    if err:
        sys.stderr.write(err)
    print(f"\nRemote exit code: {code}")
    ssh.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
