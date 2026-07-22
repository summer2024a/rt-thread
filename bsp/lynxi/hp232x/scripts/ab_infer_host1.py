#!/usr/bin/env python3
"""
Host1 (192.168.58.36) board2 chip30/31 llama A/B: hp640 vs RTT (flash cold boot).

run_rtt.sh: inferPerf 2-chip @ global chips 95 94 (= board2 chip31, chip30).
Only chip30 has UART; chip31 stays hp640.

Usage (dev machine):
  python3 scripts/ab_infer_host1.py --remote --loops 30
"""
from __future__ import print_function

import argparse
import os
import shlex
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BSP_DIR = os.path.dirname(SCRIPT_DIR)

DEFAULT_HOST = "192.168.58.36"
DEFAULT_USER = "lynxi"
DEFAULT_PASS = "lx@123"
FIFO_DIR = "/home/lynxi/xia/fifo_test"
KA200_DIR = "/home/lynxi/xia/ka200"
LOGDIR = "/tmp/apu_infer_ab"

# Flash images on test host (or NFS)
HP640_BIN = "hp232x_ka200_4.11.bin"
RTT_BIN_NFS = "/mnt/49.20/lynxlink/lynxi-rtt/bsp/lynxi/hp232x/HP232x_KA200_Serdes_Update_20260720_v5.0.bin"


def run_local(cmd, timeout=600):
    print("[cmd] %s" % cmd, flush=True)
    return subprocess.run(
        cmd, shell=True, timeout=timeout,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def remote_main(args):
    import shlex as _shlex

    password = args.password
    user = args.user
    host = args.host

    def ssh(cmd, timeout=900):
        full = "sshpass -p %s ssh -o StrictHostKeyChecking=no %s@%s %s" % (
            _shlex.quote(password), user, host, _shlex.quote(cmd))
        return run_local(full, timeout=timeout)

    def scp_to(src, dst):
        full = "sshpass -p %s scp -o StrictHostKeyChecking=no %s %s@%s:%s" % (
            _shlex.quote(password), _shlex.quote(src), user, host,
            _shlex.quote(dst))
        return run_local(full, timeout=120)

    # Ensure latest RTT image on NFS is linked into ka200/
    rtt_local = os.path.join(BSP_DIR, "HP232x_KA200_Serdes_Update_20260720_v5.0.bin")
    if os.path.isfile(rtt_local):
        scp_to(rtt_local, KA200_DIR + "/HP232x_KA200_Serdes_Update_20260720_v5.0.bin")

    # Upload this script and a board-side runner
    runner = os.path.join(SCRIPT_DIR, "_ab_infer_board.sh")
    scp_to(runner, "/tmp/_ab_infer_board.sh")
    ssh("chmod +x /tmp/_ab_infer_board.sh")

    cmd = ("sudo -n true 2>/dev/null; "
           "bash /tmp/_ab_infer_board.sh --loops %d --logdir %s "
           "--hp640 %s/%s --rtt %s/HP232x_KA200_Serdes_Update_20260720_v5.0.bin "
           "--fifo %s" % (
               args.loops, LOGDIR, KA200_DIR, HP640_BIN, KA200_DIR, FIFO_DIR))
    r = ssh(cmd, timeout=args.loops * 120 + 600)
    print(r.stdout or "", flush=True)
    # Pull logs
    for name in ("hp640_infer.log", "rtt_infer.log", "summary.txt",
                 "hp640_topo.txt", "rtt_topo.txt"):
        run_local(
            "sshpass -p %s scp -o StrictHostKeyChecking=no %s@%s:%s/%s %s/" % (
                _shlex.quote(password), user, host, LOGDIR, name,
                _shlex.quote(args.out)),
            timeout=60)
    return r.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--remote", action="store_true")
    ap.add_argument("--host", default=os.environ.get("HP232X_TEST_HOST", DEFAULT_HOST))
    ap.add_argument("--user", default=DEFAULT_USER)
    ap.add_argument("--password", default=DEFAULT_PASS)
    ap.add_argument("--loops", type=int, default=30)
    ap.add_argument("--out", default="/tmp/apu_infer_ab_local")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    if args.remote:
        return remote_main(args)

    print("Run with --remote from dev machine", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main() or 0)
