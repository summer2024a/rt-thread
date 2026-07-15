#!/usr/bin/env python3
"""
Run HP232X board test from dev machine via SSH to 192.168.58.36.

Usage:
    python3 remote_board_test.py smp          # SMP / Core1 test
    python3 remote_board_test.py biz          # eMMC biz test
    python3 remote_board_test.py flash        # generic flash + log
    python3 remote_board_test.py upload       # upload firmware only

Requires: paramiko, sshpass optional; test host has pyserial.
"""

import argparse
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BSP_DIR = os.path.dirname(SCRIPT_DIR)

SERVER = os.environ.get("HP232X_TEST_HOST", "192.168.58.36")
USER = os.environ.get("HP232X_TEST_USER", "lynxi")
PASS = os.environ.get("HP232X_TEST_PASS", "lx@123")
SUDO = PASS

FW_LOCAL = os.path.join(BSP_DIR, "rtthread-header.bin")
# Avoid NFS shared mount — scp to local xmodem dir on test host
FW_REMOTE = "/home/lynxi/xia/xmodem/rtthread-header.bin"
XMODEM_DIR = "/home/lynxi/xia/xmodem"
SCRIPTS_REMOTE = "/home/lynxi/xia/xmodem/scripts/hp232x"

MODE_SCRIPT = {
    "smp": "flash_run_smp.py",
    "biz": "flash_run_biz0.py",
    "flash": "flash_and_log.py",
}


def ssh_connect():
    import paramiko
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(SERVER, username=USER, password=PASS, timeout=15)
    return ssh


def run(ssh, cmd, timeout=300):
    stdin, stdout, stderr = ssh.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode(errors="replace")
    err = stderr.read().decode(errors="replace")
    rc = stdout.channel.recv_exit_status()
    return rc, out, err


def upload_firmware(ssh):
    if not os.path.isfile(FW_LOCAL) or os.path.getsize(FW_LOCAL) < 1024:
        print("ERROR: %s missing or too small — run: cd %s && scons && python3 mkimage.py ..." % (
            FW_LOCAL, BSP_DIR))
        return 1
    sftp = ssh.open_sftp()
    sftp.put(FW_LOCAL, FW_REMOTE)
    sftp.close()
    print("[remote] uploaded %s -> %s (%d bytes)" % (
        FW_LOCAL, FW_REMOTE, os.path.getsize(FW_LOCAL)))
    rc, out, err = run(ssh, """
cd %s
ln -sf %s u-boot-spl.bin
python3 -c "
import struct, os
fw='%s'
assert os.path.getsize(fw) > 1024, 'firmware empty'
f=open(fw,'rb'); f.seek(8); dest=struct.unpack('<I',f.read(4))[0]; f.close()
spl=dest & ~0xFFF
w=open('boot-wrapper.bin','r+b'); w.seek(8); w.write(struct.pack('<Q',spl)); w.close()
print('spl=0x%%08X size=%%d' %% (spl, os.path.getsize(fw)))
"
ls -lh u-boot-spl.bin rtthread-header.bin
""" % (XMODEM_DIR, FW_REMOTE, FW_REMOTE))
    print(out.strip())
    return rc


def sync_scripts(ssh):
    """Rsync scripts/ to test host (via tar over SSH)."""
    import tarfile
    import io
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name in os.listdir(SCRIPT_DIR):
            if name.endswith(".py"):
                tar.add(os.path.join(SCRIPT_DIR, name), arcname=name)
    buf.seek(0)
    sftp = ssh.open_sftp()
    run(ssh, "mkdir -p %s" % SCRIPTS_REMOTE)
    sftp.putfo(buf, "/tmp/hp232x_scripts.tgz")
    sftp.close()
    run(ssh, "cd %s && tar xzf /tmp/hp232x_scripts.tgz" % SCRIPTS_REMOTE)
    print("[remote] scripts synced to %s" % SCRIPTS_REMOTE)


def run_test(ssh, mode):
    script = MODE_SCRIPT.get(mode)
    if not script:
        print("unknown mode: %s" % mode)
        return 1
    sync_scripts(ssh)
    if upload_firmware(ssh) != 0:
        return 1
    cmd = (
        "echo '%s' | sudo -S fuser -k /dev/ttyUSB0 2>/dev/null; sleep 1; "
        "cd %s && echo '%s' | sudo -S python3 %s"
        % (SUDO, SCRIPTS_REMOTE, SUDO, script)
    )
    rc, out, err = run(ssh, cmd, timeout=240)
    sys.stdout.write(out)
    if err and "password" not in err.lower():
        sys.stderr.write(err)
    return rc


def main():
    parser = argparse.ArgumentParser(description="HP232X remote board test @ 58.36")
    parser.add_argument("mode", choices=["smp", "biz", "flash", "upload"],
                        help="test mode")
    args = parser.parse_args()
    ssh = ssh_connect()
    try:
        if args.mode == "upload":
            return upload_firmware(ssh)
        return run_test(ssh, args.mode)
    finally:
        ssh.close()


if __name__ == "__main__":
    sys.exit(main() or 0)
