#!/usr/bin/env python3
"""
Send one file with ZMODEM over the HP232X debug UART (test host 58.36).

Requires lrzsz on the host:
    sudo apt install lrzsz

Typical flash update flow (board already at msh):
    sudo python3 send_zmodem.py --cmd 'flash update 0x00 0x4000' foo.bin

After transfer, keeps listening for:
    flash update: OK NOR@...     → PASS
    flash update: write fail...  → FAIL

Detach screen/minicom first (Ctrl-A d). If msh shows raw ``**B0…`` while
the script runs, screen is stealing ZRINIT and sz will hang/timeout.

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
    zmodem_send,
)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def main():
    os.chdir(SCRIPT_DIR)
    ap = argparse.ArgumentParser(description="ZMODEM send one file + wait flash result")
    ap.add_argument("file", help="path to file to send")
    ap.add_argument("-d", "--device", default=SERIAL_DEV,
                    help="serial device (default %(default)s)")
    ap.add_argument("-b", "--baud", type=int, default=BAUD)
    ap.add_argument("--cmd", default=None,
                    help="msh command before transfer "
                         "(e.g. \"flash update 0x00 0x4000\")")
    ap.add_argument("--cmd-wait", type=float, default=0.8,
                    help="seconds after --cmd before ZMODEM (default 0.8)")
    ap.add_argument("--wait-flash", action="store_true", default=True,
                    help="after send, wait for flash update OK/FAIL (default)")
    ap.add_argument("--no-wait-flash", action="store_false", dest="wait_flash",
                    help="exit right after ZMODEM (do not wait NOR program)")
    ap.add_argument("--flash-timeout", type=float, default=3.0,
                    help="seconds to wait for flash OK/FAIL (default 3; "
                         "raise for large NOR ranges)")
    ap.add_argument("--log", default=None, help="optional log file path")
    args = ap.parse_args()

    if not os.path.isfile(args.file):
        print("missing file: %s" % args.file, file=sys.stderr)
        return 2

    level, msg = check_cmd_matches_sender(args.cmd, "zmodem")
    if level == "error":
        print("[zmodem] ERROR: %s" % msg, file=sys.stderr)
        return 2
    if level == "warn":
        print("[zmodem] WARN: %s" % msg, flush=True)

    ser = open_serial(args.device, args.baud)
    try:
        drain(ser, 0.3)
        if args.cmd:
            # Keep ZRINIT / 'C' — do not drain after cmd.
            send_line(ser, args.cmd, wait_s=args.cmd_wait, drain_after=False)
        zmodem_send(ser, args.file)
        if args.wait_flash:
            ok = wait_flash_update_result(
                ser, timeout_s=args.flash_timeout, log_path=args.log)
            if ok is None:
                print("[zmodem] done (CHECK screen for flash OK)")
                return 0
            print("[zmodem] done (%s)" % ("PASS" if ok else "FAIL"))
            return 0 if ok else 1
    finally:
        try:
            ser.close()
        except Exception:
            pass
        # Prevent leftover board ZRINIT (**B08…) from becoming a bash command.
        flush_stdin_typeahead()
    print("[zmodem] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
