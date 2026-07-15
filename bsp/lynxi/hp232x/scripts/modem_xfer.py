#!/usr/bin/env python3
"""
YMODEM / ZMODEM single-file send helpers for HP232X test host (58.36).

Same env/serial conventions as flash_common.py:
    HP232X_SERIAL   /dev/ttyUSB0
    HP232X_BAUD     115200

YMODEM: prefer lrzsz ``sb`` (auto); pure-Python fallback with corrected
  block0 handshake (bare 'C' is NOT ACK).
ZMODEM: wraps lrzsz ``sz``.

ZMODEM notes:
  Board ZRINIT looks like ``**B0800000000022d``. Do not echo raw modem
  frames to the host shell (becomes ``command not found``). ``zmodem_send``
  opens the USB TTY as sz stdin/stdout and strips protocol noise.
"""

from __future__ import print_function

import os
import re
import shutil
import subprocess
import sys
import time

import serial

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

SERIAL_DEV = os.environ.get("HP232X_SERIAL", "/dev/ttyUSB0")
BAUD = int(os.environ.get("HP232X_BAUD", "115200"))

SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRC = 0x43  # 'C'

# Board ZRINIT / ZMODEM hdr looks like "**B0800000000022d" (hex hdr + CRC).
# If echoed to the host shell, bash treats it as a command → "command not found".
_ZMODEM_HDR_RE = re.compile(
    br"\*{2}[\x18]?[BbCc][0-9A-Fa-f]{10,32}"
)
_SZ_NOISE_RE = re.compile(
    rb"(Starting zmodem transfer[^\n]*\n?)|"
    rb"(Transfer incomplete[^\n]*\n?)|"
    rb"(\*{2}[\x18]?[BbCc][0-9A-Fa-f]{8,40})"
)


def open_serial(dev=None, baud=None, timeout=0.05, exclusive=None):
    """Open UART. Default **exclusive** so screen/minicom cannot steal bytes.

    Screen Ymodem: run ``flash updatey`` in msh, send via screen — no script.
    Script path: detach screen first, or set HP232X_SERIAL_EXCLUSIVE=0 to share
    (fragile; may hit "multiple access on port").
    """
    kwargs = dict(port=dev or SERIAL_DEV, baudrate=baud or BAUD, timeout=timeout)
    if exclusive is None:
        exclusive = os.environ.get("HP232X_SERIAL_EXCLUSIVE", "1") not in (
            "0", "false", "False")
    if exclusive:
        try:
            return serial.Serial(exclusive=True, **kwargs)
        except TypeError:
            pass
        except (OSError, serial.SerialException) as e:
            raise serial.SerialException(
                "%s — is screen/minicom still on %s? Detach it, or "
                "HP232X_SERIAL_EXCLUSIVE=0 (share, unreliable)" %
                (e, kwargs["port"]))
    return serial.Serial(**kwargs)


def _serial_read(ser, size=4096):
    """Read; return b'' on timeout. Raise only unexpected errors.

    Contended TTY (script + screen) often yields SerialException — convert to
    a soft error the caller can handle.
    """
    try:
        return ser.read(size)
    except serial.SerialException as e:
        msg = str(e).lower()
        if "multiple access" in msg or "disconnected" in msg or "ready" in msg:
            raise serial.SerialException(
                "UART contention with another process (screen/minicom?). "
                "Detach screen before send_ymodem/send_zmodem, or check msh "
                "for 'flash update: OK NOR'. (%s)" % e)
        raise


def drain(ser, seconds=0.2):
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = _serial_read(ser, 4096)
        except serial.SerialException:
            return
        if not chunk:
            time.sleep(0.02)


def flush_stdin_typeahead():
    """Drop leftover ZMODEM bytes that landed on the controlling TTY stdin.

    After sz <-> /dev/ttyUSBx, board ZRINIT retries can leak into the local
    shell and become `**0800000000022d: command not found`.
    """
    try:
        import termios
        if sys.stdin is not None and sys.stdin.isatty():
            termios.tcflush(sys.stdin.fileno(), termios.TCIFLUSH)
    except Exception:
        pass


def echo_uart_clean(data):
    """Print UART bytes to stdout, stripping ZMODEM frames / control junk."""
    if not data:
        return
    data = _ZMODEM_HDR_RE.sub(b"", data)
    out = bytearray()
    for b in data:
        if b in (0x09, 0x0A, 0x0D) or 0x20 <= b <= 0x7E:
            out.append(b)
        # drop CAN/SOH/STX and other binary modem controls
    if out:
        sys.stdout.buffer.write(bytes(out))
        sys.stdout.flush()


def wait_for_marker(ser, marker, timeout_s=30.0):
    """Read until UTF-8/ASCII marker appears (still prints to stdout)."""
    if isinstance(marker, str):
        marker = marker.encode("ascii")
    print("[modem] wait for %r ..." % marker, flush=True)
    buf = b""
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            chunk = _serial_read(ser, 256)
        except serial.SerialException as e:
            raise TimeoutError("serial error waiting for %r: %s" % (marker, e))
        if chunk:
            buf += chunk
            echo_uart_clean(chunk)
            if marker in buf:
                return True
            if len(buf) > 8192:
                buf = buf[-4096:]
        else:
            time.sleep(0.02)
    raise TimeoutError("timeout waiting for %r" % marker)


def send_line(ser, line, wait_s=0.3, drain_after=True):
    """Send a msh/finsh command line.

    For Y/Zmodem: use drain_after=False so handshake 'C'/ZRINIT is not eaten.
    """
    if not line.endswith("\n"):
        line = line + "\n"
    data = line.encode("utf-8", errors="replace")
    print("[modem] cmd: %s" % line.rstrip(), flush=True)
    ser.write(data)
    ser.flush()
    time.sleep(wait_s)
    if drain_after:
        drain(ser, 0.15)


def _crc16_ccitt(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def _read_byte(ser, timeout_s):
    old = ser.timeout
    ser.timeout = timeout_s
    try:
        b = ser.read(1)
        return b[0] if b else None
    finally:
        ser.timeout = old


def _wait_byte(ser, wanted, timeout_s=60.0, name="byte", max_discard=None):
    """Wait until one of wanted bytes (set/list) appears; discard others.

    max_discard: if set, raise when too many non-matching bytes arrive first.
    Prevents mistaking payload/noise 0x06 for a real ACK while desynced.
    """
    if isinstance(wanted, int):
        wanted = (wanted,)
    end = time.time() + timeout_s
    discarded = 0
    while time.time() < end:
        b = _read_byte(ser, 0.2)
        if b is None:
            continue
        if b in wanted:
            return b
        if b == CAN:
            raise RuntimeError("%s: receiver sent CAN" % name)
        discarded += 1
        if max_discard is not None and discarded > max_discard:
            raise RuntimeError(
                "%s: discarded %d bytes before %s (desync? last=0x%02x)" %
                (name, discarded, wanted, b))
    raise TimeoutError("%s: timeout waiting for %s" % (name, wanted))


def _write_paced(ser, data, chunk=32, gap_s=0.003):
    """Write in small chunks so boards with tiny UART rings (e.g. 128B) do not overrun.

    SOH frame is 133B; if host dumps it in one write against RT_SERIAL_RB_BUFSZ=128,
    the tail is dropped → board RYM_ERR_CODE (-113).
    Current hp232x uses RB=2048 → prefer chunk<=0 (one-shot).
    """
    if chunk <= 0 or len(data) <= chunk:
        ser.write(data)
        ser.flush()
        return
    for i in range(0, len(data), chunk):
        ser.write(data[i:i + chunk])
        ser.flush()
        if i + chunk < len(data) and gap_s > 0:
            time.sleep(gap_s)


def _send_block(ser, seq, payload, use_1k=False, pace_chunk=0, pace_gap_s=0.003):
    if use_1k:
        header = bytes([STX, seq & 0xFF, (~seq) & 0xFF])
        if len(payload) < 1024:
            payload = payload + b"\x1a" * (1024 - len(payload))
        elif len(payload) > 1024:
            payload = payload[:1024]
    else:
        header = bytes([SOH, seq & 0xFF, (~seq) & 0xFF])
        if len(payload) < 128:
            payload = payload + b"\x1a" * (128 - len(payload))
        elif len(payload) > 128:
            payload = payload[:128]
    crc = _crc16_ccitt(payload)
    frame = header + payload + bytes([(crc >> 8) & 0xFF, crc & 0xFF])
    _write_paced(ser, frame, chunk=pace_chunk, gap_s=pace_gap_s)


def ymodem_send_pure(ser, path, timeout_s=90.0, use_1k=False, pkt_gap_s=0.01,
                     pace_chunk=0, pace_gap_s=0.003):
    """
    Pure-Python YMODEM-CRC sender.

    Defaults assume board RT_SERIAL_RB_BUFSZ>=256 (hp232x=2048): one-shot
    writes (pace_chunk=0). Use pace_chunk=32 only on old RB=128 images.

    Important: after block0, only ACK (then C) means success. A bare 'C' means
    the board is still handshaking — resend block0. Accepting bare 'C' caused
    host-side false OK while board failed with stage=2 err=-113 recv=0.
    """
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise FileNotFoundError(path)

    fname = os.path.basename(path)
    fsize = os.path.getsize(path)
    print("[ymodem] pure-python %s (%d bytes) → %s (block=%s gap=%.0fms pace=%d/%gms)" %
          (path, fsize, ser.port, "1k" if use_1k else "128", pkt_gap_s * 1000,
           pace_chunk, pace_gap_s * 1000),
          flush=True)

    # Wait for receiver 'C' (banner text before 'C' is discarded)
    _wait_byte(ser, CRC, timeout_s=timeout_s, name="ymodem handshake")
    # Brief settle so board is inside rym handshake read, not mid-printf
    time.sleep(0.05)

    # Block 0: name + size
    meta = fname.encode("utf-8") + b"\x00" + str(fsize).encode("ascii") + b"\x00"
    blk0_ok = False
    for _attempt in range(15):
        time.sleep(pkt_gap_s)
        _send_block(ser, 0, meta, use_1k=False,
                    pace_chunk=pace_chunk, pace_gap_s=pace_gap_s)
        b = _wait_byte(ser, (ACK, NAK, CRC), timeout_s=10.0,
                       name="ymodem blk0", max_discard=64)
        if b == ACK:
            # Expect second 'C' (start of data phase); tolerate missing
            try:
                _wait_byte(ser, CRC, timeout_s=3.0, name="ymodem data-C",
                           max_discard=8)
            except (TimeoutError, RuntimeError):
                pass
            blk0_ok = True
            break
        if b == CRC:
            # Still in handshake — do NOT treat as success
            print("[ymodem] blk0: got C (handshake retry), resend", flush=True)
            continue
        if b == NAK:
            print("[ymodem] blk0: NAK, resend", flush=True)
            continue
    if not blk0_ok:
        raise RuntimeError("ymodem: block0 not ACKed")

    seq = 1
    sent = 0
    blk = 1024 if use_1k else 128
    with open(path, "rb") as f:
        while sent < fsize:
            chunk = f.read(blk)
            if not chunk:
                break
            for _attempt in range(10):
                time.sleep(pkt_gap_s)
                _send_block(ser, seq, chunk, use_1k=use_1k,
                            pace_chunk=pace_chunk, pace_gap_s=pace_gap_s)
                b = _wait_byte(ser, (ACK, NAK), timeout_s=10.0,
                               name="ymodem data", max_discard=32)
                if b == ACK:
                    break
                if b == NAK:
                    continue
            else:
                raise RuntimeError("ymodem: seq=%d not ACKed" % seq)
            sent += len(chunk)
            seq = (seq + 1) & 0xFF
            if seq == 0:
                seq = 1
            if (sent & 0x7FFF) < len(chunk):
                print("[ymodem] %d / %d" % (sent, fsize), flush=True)

    # EOT handshake (classic: NAK then second EOT → ACK)
    got_ack = False
    for _attempt in range(10):
        time.sleep(pkt_gap_s)
        ser.write(bytes([EOT]))
        ser.flush()
        b = _wait_byte(ser, (ACK, NAK), timeout_s=10.0, name="ymodem EOT",
                       max_discard=16)
        if b == ACK:
            got_ack = True
            break
        # NAK → send EOT again
    if not got_ack:
        raise RuntimeError("ymodem: EOT not ACKed")

    # Empty block 0 (batch end)
    try:
        _wait_byte(ser, CRC, timeout_s=3.0, name="ymodem end-C", max_discard=16)
    except (TimeoutError, RuntimeError):
        pass
    time.sleep(pkt_gap_s)
    _send_block(ser, 0, b"", use_1k=False,
                pace_chunk=pace_chunk, pace_gap_s=pace_gap_s)
    try:
        _wait_byte(ser, ACK, timeout_s=5.0, name="ymodem end-ACK", max_discard=16)
    except (TimeoutError, RuntimeError):
        print("[ymodem] WARN: no final ACK (often OK)", flush=True)

    print("[ymodem] OK sent %d bytes" % fsize, flush=True)
    flush_stdin_typeahead()
    return fsize


def ymodem_send_sb(ser, path, timeout_s=120.0):
    """
    Send one file with YMODEM via lrzsz ``sb`` (same fd pattern as ``sz``).

    Board must already be in rym recv (flash updatey).
    If only ``sz`` is installed, invoke it with argv0=``sb`` so lrzsz
    selects YMODEM (Debian ships /usr/bin/sb → sz).
    """
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise FileNotFoundError(path)

    sb = shutil.which("sb")
    sz = shutil.which("sz")
    if sb:
        exe = sb
        argv0 = "sb"
    elif sz:
        exe = sz
        argv0 = "sb"  # argv[0] selects YMODEM in lrzsz
    else:
        raise RuntimeError(
            "ymodem needs lrzsz (`sb` or `sz`). On 58.36: sudo apt install lrzsz")

    port = ser.port
    baud = ser.baudrate
    fsize = os.path.getsize(path)
    print("[ymodem] send %s (%d bytes) via %s (as %s) → %s" %
          (path, fsize, exe, argv0, port), flush=True)

    ser.close()
    try:
        argv = [argv0, "-e", "-y", path]
        _lrzsz_on_port(argv, port, baud, timeout_s, what="ymodem", executable=exe)
    finally:
        time.sleep(0.15)
        ser.port = port
        ser.baudrate = baud
        ser.timeout = 0.05
        ser.open()
        flush_stdin_typeahead()

    print("[ymodem] OK", flush=True)
    return fsize

def ymodem_send(ser, path, timeout_s=90.0, use_1k=False, pkt_gap_s=0.01,
                pace_chunk=0, pace_gap_s=0.003, engine="auto"):
    """
    Send one file with YMODEM.

    engine:
      auto  — prefer lrzsz ``sb``/``sz --ymodem`` (recommended; same as ZMODEM)
      sb    — force lrzsz
      pure  — pure Python (legacy; need correct pacing on small RB)
    """
    engine = (engine or "auto").lower()
    if engine == "auto":
        if shutil.which("sb") or shutil.which("sz"):
            engine = "sb"
        else:
            engine = "pure"
            print("[ymodem] WARN: no lrzsz; falling back to pure-python",
                  flush=True)

    if engine == "sb":
        return ymodem_send_sb(ser, path, timeout_s=timeout_s)
    if engine == "pure":
        return ymodem_send_pure(
            ser, path, timeout_s=timeout_s, use_1k=use_1k,
            pkt_gap_s=pkt_gap_s, pace_chunk=pace_chunk, pace_gap_s=pace_gap_s)
    raise ValueError("ymodem engine must be auto|sb|pure, got %r" % engine)


def tty_other_pids(port):
    """PIDs (besides us) that currently have ``port`` open."""
    try:
        r = subprocess.run(
            ["fuser", port],
            capture_output=True, text=True, timeout=2)
    except (OSError, subprocess.TimeoutExpired):
        return []
    text = "%s %s" % (r.stdout or "", r.stderr or "")
    me = os.getpid()
    pids = []
    for tok in text.replace(port, " ").split():
        if tok.isdigit():
            p = int(tok)
            if p != me and p not in pids:
                pids.append(p)
    return pids


def _lrzsz_on_port(argv, port, baud, timeout_s, what="modem", executable=None):
    """
    Run lrzsz against USB UART with **two** FDs (stdin + stdout).

    One shared FD often makes sz hang forever on ZRINIT with USB-serial.
    Also refuse if another process (screen) still holds the port.
    """
    busy = tty_other_pids(port)
    if busy:
        raise RuntimeError(
            "%s: %s busy (pids %s). Detach screen/minicom first (Ctrl-A d). "
            "If msh shows raw **B0… ZRINIT, screen ate the handshake — "
            "re-issue flash update after detach." % (what, port, busy))

    subprocess.run([
        "stty", "-F", port, str(baud), "cs8", "-cstopb", "-parenb",
        "raw", "-echo", "-ixon", "-ixoff",
    ], check=False)

    print("[%s] %s <> %s (dual-fd)" % (what, " ".join(argv), port), flush=True)
    fd_in = os.open(port, os.O_RDWR | os.O_NOCTTY)
    fd_out = os.open(port, os.O_RDWR | os.O_NOCTTY)
    try:
        try:
            import fcntl
            import termios
            fcntl.ioctl(fd_in, termios.TIOCEXCL)
            fcntl.ioctl(fd_out, termios.TIOCEXCL)
        except Exception:
            pass
        run_kw = dict(
            stdin=fd_in,
            stdout=fd_out,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
        )
        if executable:
            run_kw["executable"] = executable
        try:
            r = subprocess.run(argv, **run_kw)
        except subprocess.TimeoutExpired as e:
            raise RuntimeError(
                "%s: timed out after %.0fs. Detach screen if open — "
                "raw **B0… on msh means screen stole ZRINIT from sz/sb." %
                (what, timeout_s)) from e
    finally:
        for fd in (fd_in, fd_out):
            try:
                os.close(fd)
            except OSError:
                pass

    if r.stderr:
        clean = _SZ_NOISE_RE.sub(b"", r.stderr)
        printable = bytearray()
        for b in clean:
            if b in (0x09, 0x0A, 0x0D) or 0x20 <= b <= 0x7E:
                printable.append(b)
        clean = bytes(printable).strip()
        if clean:
            sys.stderr.buffer.write(clean)
            if not clean.endswith(b"\n"):
                sys.stderr.buffer.write(b"\n")
            sys.stderr.flush()
    if r.returncode != 0:
        raise RuntimeError("%s: lrzsz exit %d" % (what, r.returncode))
    return r


def check_cmd_matches_sender(cmd, sender):
    """
    sender: 'ymodem' | 'zmodem'
    Return (severity, message). level is None | 'warn' | 'error'.

    History:
      - Older FW: flash update  = YMODEM only
      - Newer FW: flash update  = ZMODEM, flash updatey = YMODEM
    So YMODEM + 'flash update' is valid for legacy images — warn only.
    """
    if not cmd:
        return None, None
    c = cmd.strip().lower()
    if not c.startswith("flash update"):
        return None, None
    is_updatey = c.startswith("flash updatey")
    if sender == "ymodem" and not is_updatey:
        return ("warn",
                "cmd is 'flash update' (on NEW FW that is ZMODEM). "
                "OK for OLD YMODEM-only FW; on NEW FW use "
                "'flash updatey' or send_zmodem.py")
    if sender == "zmodem" and is_updatey:
        return ("error",
                "cmd is 'flash updatey' (YMODEM); use send_ymodem.py")
    return None, None


def zmodem_send(ser, path, timeout_s=60.0):
    """
    Send one file with ZMODEM via lrzsz `sz`.
    Closes/reopens around sz: sz owns the TTY briefly.
    Receiver must already be in ZMODEM recv (e.g. flash update).

    Detach screen first — shared TTY steals ZRINIT and sz hangs.
    """
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise FileNotFoundError(path)

    sz = shutil.which("sz")
    if not sz:
        raise RuntimeError(
            "zmodem needs lrzsz (`sz`). On 58.36: sudo apt install lrzsz")

    port = ser.port
    baud = ser.baudrate
    fsize = os.path.getsize(path)
    print("[zmodem] send %s (%d bytes) via %s → %s" %
          (path, fsize, sz, port), flush=True)

    ser.close()
    try:
        argv = [
            sz, "--zmodem", "-e", "-y",
            "-l", "1024", "-w", "1024",
            path,
        ]
        _lrzsz_on_port(argv, port, baud, timeout_s, what="zmodem")
    finally:
        time.sleep(0.15)
        ser.port = port
        ser.baudrate = baud
        ser.timeout = 0.05
        ser.open()
        flush_stdin_typeahead()

    print("[zmodem] OK", flush=True)
    return fsize



def capture_serial(ser, seconds, log_path=None):
    print("[modem] capture %ds" % seconds, flush=True)
    logf = open(log_path, "wb") if log_path else None
    try:
        end = time.time() + seconds
        while time.time() < end:
            data = ser.read(4096)
            if data:
                if logf:
                    logf.write(data)
                    logf.flush()
                echo_uart_clean(data)
            else:
                time.sleep(0.01)
    finally:
        if logf:
            logf.close()
            print("[modem] log: %s" % log_path, flush=True)


# Board prints after modem recv (biz_finsh_cmds flash_cmd_update)
_FLASH_OK = (
    b"flash update: OK NOR",
)
_FLASH_FAIL = (
    b"flash update: write fail",
    b"flash update: Ymodem fail",
    b"flash update: Zmodem fail",
    b"flash update: bringup failed",
    b"flash update: protocol not built-in",
    b"flash update: no console device",
)


def wait_flash_update_result(ser, timeout_s=3.0, log_path=None):
    """
    After Y/Zmodem finishes, read UART until flash programming reports OK/FAIL.

    Default timeout is short (3s). Returns:
        True   — saw "flash update: OK NOR..."
        False  — saw a fail line or timeout
        None   — UART contention (e.g. screen still attached); check msh/screen
    """
    print("[modem] wait flash result (timeout %.1fs)..." % timeout_s, flush=True)
    buf = b""
    logf = open(log_path, "ab") if log_path else None
    end = time.time() + timeout_s
    try:
        while time.time() < end:
            try:
                data = _serial_read(ser, 4096)
            except serial.SerialException as e:
                print("\n[modem] WARN: %s" % e, flush=True)
                print("[modem] → look in screen/msh for "
                      "'flash update: OK NOR' / fail line", flush=True)
                flush_stdin_typeahead()
                return None
            if data:
                buf += data
                if logf:
                    logf.write(data)
                    logf.flush()
                echo_uart_clean(data)
                for pat in _FLASH_OK:
                    if pat in buf:
                        print("\n[modem] PASS: flash write OK", flush=True)
                        flush_stdin_typeahead()
                        return True
                for pat in _FLASH_FAIL:
                    if pat in buf:
                        print("\n[modem] FAIL: %s" % pat.decode("ascii", "replace"),
                              flush=True)
                        flush_stdin_typeahead()
                        return False
            else:
                time.sleep(0.02)
    finally:
        if logf:
            logf.close()
            print("[modem] log: %s" % log_path, flush=True)

    print("\n[modem] FAIL: timeout waiting for flash update result "
          "(need 'flash update: OK NOR...')", flush=True)
    flush_stdin_typeahead()
    return False