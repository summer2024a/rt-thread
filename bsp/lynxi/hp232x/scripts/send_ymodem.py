#!/usr/bin/env python3
"""
Send one file with YMODEM over the HP232X debug UART (test host 58.36).

Board protocol (current FW):
    flash updatey <addr> <range>  ← YMODEM  (this script)
    flash update  <addr> <range>  ← ZMODEM  → send_zmodem.py

Default engine is lrzsz ``sb`` / ``sz --ymodem`` (same approach as ZMODEM).
Pure-Python fallback: ``--engine pure`` (fixed: no longer treats bare 'C'
as block0 ACK — that caused host OK + board err=-113 recv=0).

Typical:
    sudo python3 send_ymodem.py --cmd 'flash updatey 0xa6000 0x40000' foo.bin

With ``screen`` on the same UART: either
  (1) detach screen, run this script; or
  (2) keep screen, run ``flash updatey`` in msh + screen's Ymodem send.
Do not run this script while screen still holds the TTY.

Flash result wait defaults to 3s (``--flash-timeout`` for large ranges).

Requires: sudo apt install lrzsz   (for default engine)

Board needs RT_SERIAL_RB_BUFSZ >= 256 (hp232x=2048). Old RB=128 images
fail with Ymodem err=-113 — rebuild or use Host upgrade path.

Env: HP232X_SERIAL=/dev/ttyUSB0  HP232X_BAUD=115200
"""

import argparse
import os
import sys

from modem_xfer import (
    BAUD,
    SERIAL_DEV,
    check_cmd_matches_sender,
    drain,
    flush_stdin_typeahead,
    open_serial,
    send_line,
    wait_flash_update_result,
    ymodem_send,
)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def main():
    os.chdir(SCRIPT_DIR)
    ap = argparse.ArgumentParser(description="YMODEM send + wait flash result")
    ap.add_argument("file", help="path to file to send")
    ap.add_argument("-d", "--device", default=SERIAL_DEV)
    ap.add_argument("-b", "--baud", type=int, default=BAUD)
    ap.add_argument("--cmd", default=None,
                    help="msh cmd, e.g. \"flash updatey 0xa6000 0x40000\"")
    ap.add_argument("--cmd-wait", type=float, default=0.5,
                    help="settle after cmd before YMODEM handshake (default 0.5)")
    ap.add_argument("--engine", choices=("auto", "sb", "pure"), default="auto",
                    help="auto/sb=lrzsz (recommended); pure=Python sender")
    ap.add_argument("--1k", dest="use_1k", action="store_true",
                    help="pure only: use 1K STX (needs RB>=2048)")
    ap.add_argument("--pkt-gap", type=float, default=0.01,
                    help="pure only: seconds between packets (default 0.01)")
    ap.add_argument("--pace-chunk", type=int, default=0,
                    help="pure only: UART write chunk (0=one-shot, default; "
                         "32 if board RB=128)")
    ap.add_argument("--pace-gap", type=float, default=0.003,
                    help="pure only: seconds between write chunks")
    ap.add_argument("--wait-flash", action="store_true", default=True)
    ap.add_argument("--no-wait-flash", action="store_false", dest="wait_flash")
    ap.add_argument("--flash-timeout", type=float, default=3.0,
                    help="seconds to wait for flash OK/FAIL (default 3; "
                         "raise for large NOR ranges)")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    if not os.path.isfile(args.file):
        print("missing file: %s" % args.file, file=sys.stderr)
        return 2

    level, msg = check_cmd_matches_sender(args.cmd, "ymodem")
    if level == "error":
        print("[ymodem] ERROR: %s" % msg, file=sys.stderr)
        return 2
    if level == "warn":
        print("[ymodem] WARN: %s" % msg, flush=True)

    ser = open_serial(args.device, args.baud)
    try:
        drain(ser, 0.3)
        if args.cmd:
            # Do NOT drain after cmd — that eats board 'C' and the banner.
            send_line(ser, args.cmd, wait_s=args.cmd_wait, drain_after=False)

        ymodem_send(ser, args.file, use_1k=args.use_1k, pkt_gap_s=args.pkt_gap,
                    pace_chunk=args.pace_chunk, pace_gap_s=args.pace_gap,
                    engine=args.engine)
        if args.wait_flash:
            ok = wait_flash_update_result(
                ser, timeout_s=args.flash_timeout, log_path=args.log)
            if ok is None:
                print("[ymodem] done (CHECK screen for flash OK)")
                return 0
            print("[ymodem] done (%s)" % ("PASS" if ok else "FAIL"))
            return 0 if ok else 1
    finally:
        try:
            ser.close()
        except Exception:
            pass
        flush_stdin_typeahead()
    print("[ymodem] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
