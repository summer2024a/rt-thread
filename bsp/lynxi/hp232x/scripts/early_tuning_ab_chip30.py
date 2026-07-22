#!/usr/bin/env python3
"""
Early-tuning A/B on Host1 chip30 (ttyUSB0): flash-cold path preferred.

Run ON 58.36 (needs sudo for serial + ka200_tools):
  sudo python3 early_tuning_ab_chip30.py \\
    --fw /home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_YYYYMMDD_v5.0.bin

Or from dev machine after scp fw+script:
  ssh lynxi@192.168.58.36 'cd ... && echo pass | sudo -S python3 ...'
"""

from __future__ import print_function

import argparse
import os
import re
import subprocess
import sys
import time

SERIAL = os.environ.get("HP232X_SERIAL", "/dev/ttyUSB0")
BAUD = int(os.environ.get("HP232X_BAUD", "115200"))
KA200_TOOLS = "/usr/local/lynx/tools/ka200_tools"
PASS = os.environ.get("HP232X_TEST_PASS", "lx@123")


def run(cmd, timeout=180):
    print("[cmd] %s" % cmd, flush=True)
    return subprocess.run(
        cmd, shell=True, timeout=timeout,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def strip_ansi(text):
    return re.sub(r"\x1b\[[0-9;]*m", "", text or "")


def chip30_alive():
    r = run("timeout 20 lynx-showinfo", timeout=30)
    out = strip_ansi(r.stdout or "")
    m = re.search(r"\[2\].*ALIVE:([0-9A-Fa-f]+)", out)
    if not m:
        return False, out
    try:
        alive = int(m.group(1), 16)
    except ValueError:
        return False, out
    return bool(alive & (1 << 30)), out


def capture_after_reset(log_path, seconds=90):
    import serial

    run("fuser -k %s 2>/dev/null; sleep 1" % SERIAL, timeout=10)
    ser = serial.Serial(SERIAL, BAUD, timeout=0.2)
    with open(log_path, "wb") as logf:
        print("[ab] reset link0 then capture %ds → %s" % (seconds, log_path),
              flush=True)
        run("timeout -k 3 30 lynx-showinfo -r -l 0", timeout=40)
        deadline = time.time() + seconds
        while time.time() < deadline:
            data = ser.read(4096)
            if data:
                logf.write(data)
                logf.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
            else:
                time.sleep(0.02)
    ser.close()


def analyze(log_path):
    with open(log_path, "rb") as f:
        text = f.read().decode(errors="replace")
    print("\n=== early-tuning A/B analysis ===", flush=True)
    defer = "i2c deferred" in text or "BSP_I2C_DEFER=1" in text
    early = re.findall(
        r"tuning early tap=0x([0-9a-fA-F]+) iter=(\d+)", text)
    latch = re.findall(
        r"tuning latch\(post-rearm\) tap=0x([0-9a-fA-F]+) iter=(\d+)", text)
    ok = re.findall(r"tuning OK tap=0x([0-9a-fA-F]+) iter=(\d+)", text)
    tun_lines = len(re.findall(r"\[drv\] emmc tun:", text))
    mainloop = "Entering main task" in text
    i2c_start = "i2c_mcu thread started" in text or "I2C MCU slave ready" in text
    cpu1 = "emmc_biz bound to CPU1" in text or "emmc_biz entry CPU1" in text
    cpu0 = ("emmc_biz bound to CPU0" in text or "emmc_biz entry CPU0" in text or
            ("emmc_biz entry" in text and not cpu1))

    print("  I2C deferred log: %s" % defer)
    print("  I2C auto-started: %s" % i2c_start)
    print("  emmc_biz CPU1: %s  CPU0-ish: %s" % (cpu1, cpu0))
    print("  TRACE lines: %d" % tun_lines)
    print("  early re-arm: %s" % (early if early else "NONE"))
    print("  post-rearm latch: %s" % (latch if latch else "NONE"))
    print("  tuning OK: %s" % (ok if ok else "NONE"))
    print("  main loop: %s" % mainloop)

    # Pass: no early, preferably HW TUNED_CLK without latch
    if early:
        print("  VERDICT: early STILL present → current A/B not root cause")
        return 1
    if ok and not latch:
        print("  VERDICT: no early + HW TUNED_CLK (no latch) → likely factor found")
        return 0
    if ok and latch:
        print("  VERDICT: no early but still post-rearm latch (unexpected without early)")
        return 2
    print("  VERDICT: incomplete boot / tuning fail — check full log")
    return 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fw", required=True, help="Serdes_Update / Host upgrade bin")
    ap.add_argument("-l", type=int, default=0)
    ap.add_argument("-i", type=int, default=2)
    ap.add_argument("-k", type=int, default=30)
    ap.add_argument("--log", default="/tmp/early_tuning_ab_chip30.log")
    ap.add_argument("--skip-upgrade", action="store_true",
                    help="only reset+capture (fw already on flash)")
    ap.add_argument("--capture-sec", type=int, default=90)
    args = ap.parse_args()

    if not args.skip_upgrade:
        if not os.path.isfile(args.fw):
            print("missing fw: %s" % args.fw, file=sys.stderr)
            return 1
        alive, topo = chip30_alive()
        print("[ab] chip30 alive=%s" % alive, flush=True)
        if not alive:
            print("[ab] chip30 offline — cannot Host-upgrade; "
                  "use ymodem on USB0 if msh up, or --skip-upgrade after manual flash",
                  flush=True)
            print(topo[-2000:] if topo else "", flush=True)
            return 2
        cmd = (
            "timeout -k 5 180 %s -u %s -l %d -i %d -k %d"
            % (KA200_TOOLS, args.fw, args.l, args.i, args.k)
        )
        r = run(cmd, timeout=200)
        print(r.stdout or "", flush=True)
        if "Successfully" not in (r.stdout or ""):
            print("[ab] ka200_tools upgrade may have failed", flush=True)
            return 3
        print("[ab] wait 12s for FlashWrite settle", flush=True)
        time.sleep(12)

    capture_after_reset(args.log, seconds=args.capture_sec)
    return analyze(args.log)


if __name__ == "__main__":
    sys.exit(main() or 0)
