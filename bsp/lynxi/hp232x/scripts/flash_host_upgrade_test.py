#!/usr/bin/env python3
"""
Host online KA200 firmware upgrade test (hp640-compatible ka200_tools path).

ka200_tools -u 已包含：Host(FPGA) -> eMMC -> KA200 emmc_biz -> Load + FlashWrite @ 0xA6000。
无需额外 msh flash 命令。

Usage:
  python3 scripts/flash_host_upgrade_test.py
  python3 scripts/flash_host_upgrade_test.py --skip-xmodem   # KA200 已在运行
"""

import argparse
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BSP_DIR = os.path.dirname(SCRIPT_DIR)

SERVER = os.environ.get("HP232X_TEST_HOST", "192.168.58.36")
USER = os.environ.get("HP232X_TEST_USER", "lynxi")
PASS = os.environ.get("HP232X_TEST_PASS", "lx@123")
SUDO = PASS

DEFAULT_L = int(os.environ.get("HP232X_LYNXLINK", "0"))
DEFAULT_I = int(os.environ.get("HP232X_BOARD", "2"))
DEFAULT_K = int(os.environ.get("HP232X_CHIP", "30"))

KA200_DIR = "/home/lynxi/xia/ka200"
KA200_TOOLS = "/usr/local/lynx/tools/ka200_tools"
SCRIPTS_REMOTE = "/home/lynxi/xia/xmodem/scripts/hp232x"


def ssh_connect():
    import paramiko
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(SERVER, username=USER, password=PASS, timeout=15)
    return ssh


def run(ssh, cmd, timeout=300):
    i, o, e = ssh.exec_command(cmd, timeout=timeout)
    out = o.read().decode(errors="replace")
    err = e.read().decode(errors="replace")
    return o.channel.recv_exit_status(), out, err


def find_ka200_image():
    """Latest HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin under BSP_DIR."""
    import glob
    pats = sorted(glob.glob(
        os.path.join(BSP_DIR, "HP232x_KA200_Serdes_Update_*.bin")))
    return pats[-1] if pats else None


def build_ka200_image():
    import subprocess
    os.chdir(BSP_DIR)
    r = subprocess.run(["scons", "-j8"], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("scons failed:\n%s\n%s" % (r.stdout, r.stderr))
    # SConstruct PostAction runs mk_ka200_image.py → Serdes_Update_*.bin
    out = find_ka200_image()
    if not out:
        raise RuntimeError("mk_ka200_image output not found after scons")
    print("ka200 image: %s" % out)
    return out


def sync_scripts(ssh):
    import io
    import tarfile
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name in os.listdir(SCRIPT_DIR):
            if name.endswith(".py"):
                tar.add(os.path.join(SCRIPT_DIR, name), arcname=name)
    buf.seek(0)
    sftp = ssh.open_sftp()
    run(ssh, "mkdir -p %s %s" % (SCRIPTS_REMOTE, KA200_DIR))
    sftp.putfo(buf, "/tmp/hp232x_scripts.tgz")
    sftp.close()
    run(ssh, "cd %s && tar xzf /tmp/hp232x_scripts.tgz" % SCRIPTS_REMOTE)


def upload_files(ssh, ka200_local, fw_header_local):
    sftp = ssh.open_sftp()
    remote_ka200 = "%s/%s" % (KA200_DIR, os.path.basename(ka200_local))
    remote_fw = "/home/lynxi/xia/xmodem/rtthread-header.bin"
    sftp.put(ka200_local, remote_ka200)
    if fw_header_local and os.path.getsize(fw_header_local) > 1024:
        sftp.put(fw_header_local, remote_fw)
    sftp.close()
    run(ssh, """
cd /home/lynxi/xia/xmodem && ln -sf rtthread-header.bin u-boot-spl.bin
ls -lh %s u-boot-spl.bin
""" % remote_ka200)
    return remote_ka200


def xmodem_boot(ssh):
    """Legacy: xmodem only (120s log). Prefer run_host_upgrade_combined()."""
    cmd = (
        "echo '%s' | sudo -S fuser -k /dev/ttyUSB0 2>/dev/null; sleep 1; "
        "cd %s && echo '%s' | sudo -S python3 flash_run_biz0.py"
        % (SUDO, SCRIPTS_REMOTE, SUDO)
    )
    rc, out, err = run(ssh, cmd, timeout=240)
    sys.stdout.write(out)
    return rc, out


def run_host_upgrade_combined(ssh, remote_ka200, lynx, board, chip, skip_flash=False):
    """One serial session: xmodem boot -> heartbeat -> ka200_tools -u."""
    skip = " --skip-flash" if skip_flash else ""
    cmd = (
        "echo '%s' | sudo -S fuser -k /dev/ttyUSB0 2>/dev/null; sleep 1; "
        "cd %s && echo '%s' | sudo -S python3 flash_host_upgrade_run.py "
        "--ka200 %s -l %d -i %d -k %d%s 2>&1; "
        "echo '--- log tail ---'; tail -50 /tmp/hp232x_host_upgrade.log 2>/dev/null"
        % (SUDO, SCRIPTS_REMOTE, SUDO, remote_ka200, lynx, board, chip, skip)
    )
    rc, out, err = run(ssh, cmd, timeout=600)
    sys.stdout.write(out)
    if err and "password" not in err.lower():
        sys.stderr.write(err)
    return rc, out


def analyze_upgrade(out):
    checks = [
        (r"Heart-beat reported|emmc_biz entry CPU1|Entering main task",
         "KA200 heartbeat (pre-upgrade)"),
        (r"topology OK \(Link\d+ Board\d+\)", "lynx-showinfo topology OK"),
        (r"Send Update Command/Firmware Successfully", "ka200_tools host OK"),
        (r"\[biz\]\[upgrade\] OK Load", "KA200 Load (eMMC->IRAM)"),
        (r"\[biz\]\[upgrade\] OK FlashWrite",
         "KA200 FlashWrite @ 0xA6000"),
        (r"000a6000:\s+(?!ff ff)[0-9a-f]{2}",
         "Flash content @ 0xA6000 (non-erased)"),
    ]
    print("\n=== Test summary ===")
    ok = True
    for pat, label in checks:
        m = re.search(pat, out, re.I)
        if m:
            print("  [PASS] %s" % label)
        elif "Load" in label or "FlashWrite" in label:
            if re.search(r"Send Update Command/Firmware Successfully", out):
                print("  [----] %s: not in UART (check /tmp/hp232x_host_upgrade.log)"
                      % label)
                ok = False
            else:
                print("  [----] %s: not found" % label)
                ok = False
        else:
            print("  [----] %s: not found" % label)
            ok = False
    fail_m = re.search(r"\[biz\]\[upgrade\] FAIL[^\n]*", out, re.I)
    if fail_m:
        print("  [FAIL] upgrade step — %s" % fail_m.group(0)[:80])
        ok = False
    else:
        print("  [PASS] no [biz] FAIL")
    return (ok and re.search(r"Successfully", out)
            and re.search(r"topology OK", out, re.I)
            and not re.search(r"\[biz\]\[upgrade\] FAIL", out)
            and re.search(r"\[biz\]\[upgrade\] OK FlashWrite", out)
            and not re.search(r"SError", out, re.I))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-xmodem", action="store_true")
    ap.add_argument("-l", type=int, default=DEFAULT_L)
    ap.add_argument("-i", type=int, default=DEFAULT_I)
    ap.add_argument("-k", type=int, default=DEFAULT_K)
    args = ap.parse_args()

    if not args.skip_build:
        ka200 = build_ka200_image()
    else:
        ka200 = find_ka200_image()
    fw = os.path.join(BSP_DIR, "rtthread-header.bin")
    if not ka200 or not os.path.isfile(ka200) or os.path.getsize(ka200) < 1024:
        print("ERROR: no HP232x_KA200_Serdes_Update_*.bin — run without --skip-build")
        return 1
    print("using ka200 image: %s" % ka200)

    ssh = ssh_connect()
    full_log = ""
    try:
        sync_scripts(ssh)
        remote_ka200 = upload_files(ssh, ka200, fw)

        if args.skip_xmodem:
            print("\n[1] ka200_tools host upgrade (KA200 already running)...")
            rc, out = run_host_upgrade_combined(ssh, remote_ka200,
                                                args.l, args.i, args.k,
                                                skip_flash=True)
        else:
            print("\n[1] xmodem boot + host upgrade (single UART session)...")
            rc, out = run_host_upgrade_combined(ssh, remote_ka200,
                                                args.l, args.i, args.k,
                                                skip_flash=False)
        full_log += out

        ok = analyze_upgrade(full_log)
        return 0 if ok else 1
    finally:
        ssh.close()


if __name__ == "__main__":
    sys.exit(main())
