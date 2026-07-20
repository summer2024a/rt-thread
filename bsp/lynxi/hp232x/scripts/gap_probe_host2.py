#!/usr/bin/env python3
"""
FPS gap probe on host2 chip24:
  1) RTT UART boot (2x) → fpfifo CRC → serial 'phase'
  2) RTT re-boot (2x) → fpfifo -n → phase
  3) hp640 boot (1x) → fpfifo CRC → (optional) -n

Uses staging fpfifo only. One Link test at a time.
Run on test host as root (serial) or:
  python3 ab_fpfifo_host2.py --remote ...  # prefer gap_probe via --remote wrapper below

  sudo python3 gap_probe_host2.py
"""

from __future__ import print_function

import argparse
import os
import re
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

from ab_fpfifo_host2 import (  # noqa: E402
    DEFAULT_XDIR, HP640_FW, RTT_FW, RTT_MARKERS, HP640_MARKERS,
    ensure_link_exclusive, boot_fw, ensure_online_or_reset, reset_link,
    run_fpfifo, parse_avg_fps, get_topo,
)

FPFIFO = "/mnt/49.20/lynxlink/staging/fifo_test/fpfifo_stress"
SERIAL = os.environ.get("HP232X_SERIAL", "/dev/ttyUSB0")


def serial_msh_cmd(cmd, wait_s=2.0, log_path=None):
    import serial
    ser = serial.Serial(SERIAL, 115200, timeout=0.05)
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write((cmd.strip() + "\r\n").encode())
    buf = bytearray()
    end = time.time() + wait_s
    while time.time() < end:
        d = ser.read(4096)
        if d:
            buf.extend(d)
        else:
            time.sleep(0.01)
    ser.close()
    text = buf.decode("latin1", "replace")
    if log_path:
        with open(log_path, "w") as f:
            f.write(text)
    # print phase lines
    for line in text.splitlines():
        if "[phase]" in line or line.strip().startswith("msh"):
            print(line, flush=True)
    return text


def run_case(tag, fw, markers, passes, args, extra_fpfifo, logdir):
    class A:
        pass
    a = A()
    a.xdir = args.xdir
    a.fpfifo = args.fpfifo
    a.board = args.board
    a.chip = args.chip
    a.blk = args.blk
    a.test_time = args.test_time
    a.glevel = args.glevel
    a.boot_log_sec = args.boot_log_sec
    a.settle_sec = args.settle_sec
    a.reset_cmd = args.reset_cmd
    a.run_as = args.run_as

    print("\n######## CASE %s ########" % tag, flush=True)
    online, _, did = ensure_online_or_reset(a, context=" before %s" % tag)
    if did and not online:
        print("[gap] reset; need UART boot", flush=True)

    if not boot_fw(tag, args.xdir, fw, markers, passes, a, logdir):
        return None, "boot_fail", ""

    online, topo, _ = ensure_online_or_reset(a, context=" after %s boot" % tag)
    print(topo[:500], flush=True)
    if not online:
        print("[gap] offline after boot", flush=True)
        return None, "offline", ""

    # Build fpfifo cmdline manually to append -n
    others_cmd = "%s -d 0 -B %d:%d -b%d -T %d -g %d %s" % (
        args.fpfifo, args.board, args.chip, args.blk, args.test_time,
        args.glevel, extra_fpfifo)
    if args.run_as and os.geteuid() == 0:
        others_cmd = "sudo -u %s -E %s" % (args.run_as, others_cmd)
    print("[gap] %s" % others_cmd, flush=True)
    r = subprocess.run(
        others_cmd, shell=True, timeout=args.test_time + 40,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    flog = os.path.join(logdir, "%s_fpfifo.log" % tag)
    with open(flog, "w") as f:
        f.write(r.stdout or "")
    fps = parse_avg_fps(r.stdout or "")
    err = None
    out = r.stdout or ""
    if "Reduce fifo empty" in out:
        err = "reduce_empty"
    elif "离线" in out:
        err = "offline"
    print("[gap] %s FPS=%s err=%s" % (tag, fps, err), flush=True)

    phase_text = ""
    if args.phase and fw == RTT_FW:
        print("[gap] msh phase", flush=True)
        phase_text = serial_msh_cmd(
            "phase", wait_s=3.0,
            log_path=os.path.join(logdir, "%s_phase.log" % tag))

    return fps, err, phase_text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xdir", default=DEFAULT_XDIR)
    ap.add_argument("--fpfifo", default=FPFIFO)
    ap.add_argument("--board", type=int, default=0)
    ap.add_argument("--chip", type=int, default=24)
    ap.add_argument("--blk", type=int, default=16)
    ap.add_argument("--test-time", type=int, default=20)
    ap.add_argument("--glevel", type=int, default=0)
    ap.add_argument("--boot-log-sec", type=int, default=22)
    ap.add_argument("--settle-sec", type=float, default=5.0)
    ap.add_argument("--run-as", default="lynxi")
    ap.add_argument("--reset-cmd", default="lynx-showinfo -r -l 0")
    ap.add_argument("--logdir", default="/tmp/fpfifo_gap")
    ap.add_argument("--phase", action="store_true", default=True)
    ap.add_argument("--no-phase", action="store_true")
    ap.add_argument("--skip-hp640", action="store_true")
    args = ap.parse_args()
    if args.no_phase:
        args.phase = False

    os.makedirs(args.logdir, exist_ok=True)
    os.chdir(SCRIPT_DIR)
    ensure_link_exclusive(force_kill=True)

    if not os.path.isfile(args.fpfifo):
        print("ERROR: missing %s" % args.fpfifo)
        return 1

    results = {}

    # RTT with CRC (default path)
    fps, err, ph = run_case(
        "rtt_crc", RTT_FW, RTT_MARKERS, 2, args, "", args.logdir)
    results["rtt_crc"] = (fps, err, ph)

    # RTT without CRC (-n)
    fps, err, ph = run_case(
        "rtt_n", RTT_FW, RTT_MARKERS, 2, args, "-n", args.logdir)
    results["rtt_n"] = (fps, err, ph)

    if not args.skip_hp640:
        fps, err, ph = run_case(
            "hp640_crc", HP640_FW, HP640_MARKERS, 1, args, "", args.logdir)
        results["hp640_crc"] = (fps, err, ph)
        fps, err, ph = run_case(
            "hp640_n", HP640_FW, HP640_MARKERS, 1, args, "-n", args.logdir)
        results["hp640_n"] = (fps, err, ph)

    print("\n======== GAP PROBE SUMMARY ========", flush=True)
    lines = []
    for k, (fps, err, ph) in results.items():
        line = "%s FPS=%s err=%s" % (k, fps, err)
        print("  " + line, flush=True)
        lines.append(line)
        if ph:
            for pl in ph.splitlines():
                if "[phase]" in pl:
                    lines.append("  " + pl.strip())
                    print("    " + pl.strip(), flush=True)

    # Interpret
    rc = results.get("rtt_crc", (None,))[0]
    rn = results.get("rtt_n", (None,))[0]
    hc = results.get("hp640_crc", (None,))[0]
    hn = results.get("hp640_n", (None,))[0]
    print("\n-------- interpretation --------", flush=True)
    if rc and rn:
        print("RTT CRC cost share: crc_path=%.0f fps, no_crc=%.0f fps "
              "(period %.0f vs %.0f us)" %
              (rc, rn, 1e6 / rc, 1e6 / rn), flush=True)
    if hc and hn:
        print("hp640 CRC cost share: crc_path=%.0f fps, no_crc=%.0f fps "
              "(period %.0f vs %.0f us)" %
              (hc, hn, 1e6 / hc, 1e6 / hn), flush=True)
    if rc and hc:
        print("CRC gap: RTT %.1f%% slower than hp640" %
              ((hc - rc) / hc * 100), flush=True)
    if rn and hn:
        print("-n gap: RTT %.1f%% slower than hp640 (ExecBD/query/SMP)" %
              ((hn - rn) / hn * 100), flush=True)

    with open(os.path.join(args.logdir, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
