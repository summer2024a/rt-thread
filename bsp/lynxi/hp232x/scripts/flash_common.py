#!/usr/bin/env python3
"""
Shared xmodem flash + serial log capture for HP232X on 58.36 test host.

Run from this directory (scripts/ must contain xmodem.py):
    sudo python3 flash_run_smp.py

Environment (optional):
    HP232X_SERIAL      /dev/ttyUSB0
    HP232X_XMODEM_DIR  parent dir with boot-wrapper.bin + u-boot-spl.bin
    HP232X_RESET_CMD   xmodem/upgrade reset (lynx-showinfo -r -l N)
    HP232X_SHOWINFO    topology query before host upgrade (lynx-showinfo, no -r)
    HP232X_UPGRADE_RESET_CMD  alias of RESET_CMD (lynx-showinfo -r)
"""

import os
import re
import struct
import subprocess
import sys
import time

import serial
from xmodem import XMODEM

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_XMODEM_DIR = "/home/lynxi/xia/xmodem"

SERIAL_DEV = os.environ.get("HP232X_SERIAL", "/dev/ttyUSB0")
BAUD = 115200
RESET_CMD = os.environ.get(
    "HP232X_RESET_CMD",
    "lynx-showinfo -r -l 0",
)
UPGRADE_RESET_CMD = os.environ.get(
    "HP232X_UPGRADE_RESET_CMD",
    RESET_CMD,
)
UPGRADE_RESET_WAIT = float(os.environ.get("HP232X_UPGRADE_RESET_WAIT", "5"))
SHOWINFO_BIN = os.environ.get("HP232X_SHOWINFO", "lynx-showinfo")
LOG_SECONDS = int(os.environ.get("HP232X_LOG_SECONDS", "120"))


def strip_ansi(text):
    return re.sub(r"\x1b\[[0-9;]*m", "", text or "")


def check_link_topology(lynx, board, chip=None):
    """Return True when lynx-showinfo shows Link/Board ALIVE (host path ready)."""
    cmd = [SHOWINFO_BIN]
    print("[upgrade] topology check: %s" % " ".join(cmd), flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    out = (r.stdout or "") + (r.stderr or "")
    sys.stdout.write(out)
    if not out.endswith("\n"):
        sys.stdout.write("\n")
    sys.stdout.flush()
    clean = strip_ansi(out)
    if not re.search(r"\[Link%d\].*ALIVE" % lynx, clean, re.I):
        print("[upgrade] FAIL: Link%d not ALIVE in topology" % lynx, flush=True)
        return False
    if not re.search(r"\[%d\].*ALIVE" % board, clean, re.I):
        print("[upgrade] FAIL: Board%d not ALIVE in topology" % board, flush=True)
        return False
    if chip is not None and not re.search(
            r"KA200>.*\b%d\s*:" % chip, clean, re.I):
        print("[upgrade] WARN: KA200 chip %d not listed (continuing)" % chip,
              flush=True)
    print("[upgrade] topology OK (Link%d Board%d)" % (lynx, board), flush=True)
    return True


def xmodem_dir():
    return os.environ.get("HP232X_XMODEM_DIR", DEFAULT_XMODEM_DIR)


def reset_mcu(cmd=None):
    reset = cmd or RESET_CMD
    print("[flash] reset: %s" % reset, flush=True)
    subprocess.run(reset, shell=True, check=False)


def patch_boot_wrapper_spl(fw_path, wrapper_path):
    """Align boot-wrapper spl_address with rtthread-header dest_addr (4KB)."""
    with open(fw_path, "rb") as fw:
        fw.seek(8)
        dest = struct.unpack("<I", fw.read(4))[0]
    spl = dest & ~0xFFF
    with open(wrapper_path, "r+b") as f:
        f.seek(8)
        f.write(struct.pack("<Q", spl))
    print("[flash] boot-wrapper spl_address=0x%08X (dest=0x%08X)" % (spl, dest),
          flush=True)


def wait_for_xmodem(ser, timeout_s=30):
    buf = b""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
            if b".U" in buf or b"xmodem" in buf.lower():
                return True
        else:
            time.sleep(0.1)
    return False


def xmodem_send(ser, path):
    def getc(size, timeout=8):
        return ser.read(size) or None

    def putc(data, timeout=8):
        time.sleep(0.01)
        return ser.write(data) or None

    modem = XMODEM(getc, putc)
    with open(path, "rb") as stream:
        if path.endswith("spl.bin"):
            for _ in range(6):
                ser.write(b"\x00")
        else:
            ser.write(b"\x00")
        print("[flash] sending %s" % path, flush=True)
        if not modem.send(stream):
            raise RuntimeError("xmodem send failed: %s" % path)


def flash_images(xdir, reset_cmd=None):
    wrapper = os.path.join(xdir, "boot-wrapper.bin")
    spl = os.path.join(xdir, "u-boot-spl.bin")
    for p in (wrapper, spl):
        if not os.path.isfile(p):
            raise FileNotFoundError("missing %s (set HP232X_XMODEM_DIR?)" % p)
    patch_boot_wrapper_spl(spl, wrapper)
    ser = serial.Serial(SERIAL_DEV, BAUD, timeout=0.05)
    reset = reset_cmd or RESET_CMD
    reset_mcu(reset)
    wait_s = UPGRADE_RESET_WAIT if "-r" in reset else 3
    time.sleep(wait_s)
    if not wait_for_xmodem(ser):
        print("[flash] WARN: .U not seen, trying xmodem anyway", flush=True)
    xmodem_send(ser, wrapper)
    xmodem_send(ser, spl)
    return ser


def capture_boot_log(ser, log_path, seconds=LOG_SECONDS):
    print("[flash] reading boot log %ds -> %s" % (seconds, log_path), flush=True)
    with open(log_path, "wb") as logf:
        end = time.time() + seconds
        while time.time() < end:
            data = ser.read(4096)
            if data:
                logf.write(data)
                logf.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
            else:
                time.sleep(0.01)
    ser.close()
    print("[flash] log saved: %s (%d bytes)" % (log_path, os.path.getsize(log_path)),
          flush=True)


def run_flash_and_log(log_path, seconds=LOG_SECONDS, reset_cmd=None):
    os.chdir(SCRIPT_DIR)
    xdir = xmodem_dir()
    ser = flash_images(xdir, reset_cmd=reset_cmd)
    capture_boot_log(ser, log_path, seconds)
    return log_path
