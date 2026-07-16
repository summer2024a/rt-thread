#!/usr/bin/env python3
"""
MCU CLI + KA200 I2C e2e (Flash cold boot, focus soc30).

Phase order (important):
  1) KA200 reset / boot settle  — MCU must NOT enter CLI in this phase
  2) Wait MCU pending "soc rst" (if any) to finish, then KA Heart-beat + msh
  3) Confirm I2C auto-started (boot log: slave ready) — no msh i2c start
  4) MCU: +++ enter CLI (only after KA is stable)
  5) i2c_scan + ka200 reg R/W (CLI keepalive via NUL)
  6) Teardown: quit → wait 30s → lynx-showinfo -r

Does NOT change MCU rst / main-loop firmware.

  sudo python3 mcu_cli_i2c_test.py --skip-upgrade
  sudo python3 mcu_cli_i2c_test.py --skip-upgrade --no-reset   # KA already up
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("need pyserial", file=sys.stderr)
    sys.exit(2)

KA_DEV = os.environ.get("HP232X_KA_UART", "/dev/ttyUSB0")
MCU_DEV = os.environ.get("HP232X_MCU_UART", "/dev/ttyUSB1")
BAUD = 115200
CLI_KEEPALIVE_S = float(os.environ.get("HP232X_CLI_KEEPALIVE_S", "8"))
POST_QUIT_WAIT_S = float(os.environ.get("HP232X_POST_QUIT_WAIT_S", "30"))
# Extra quiet time after KA looks up, before +++ (avoid pending soc rst)
PRE_CLI_SETTLE_S = float(os.environ.get("HP232X_PRE_CLI_SETTLE_S", "5"))
SOC = int(os.environ.get("HP232X_SOC", "30"))
# Absolute MMIO addresses (protocol no longer uses base+offset)
REG_ADDR_RD = os.environ.get("HP232X_REG_ADDR_RD", "0x12500064")
# Prefer IRAM0 scratch for write/readback (byte-safe). CPR needs aligned word access.
REG_ADDR_WR = os.environ.get("HP232X_REG_ADDR_WR", "0x04020000")

KA_BOOT_MARKS = (
    b"Heart-beat reported",
    b"Entering main task",
    b"Querying tasks",
    b"msh >",
)
MCU_RST_MARKS = (b"soc rst", b"soc reset")


def ascii(b: bytes) -> str:
    return "".join(chr(c) if 32 <= c < 127 or c in (10, 13) else "." for c in b)


def drain(ser: serial.Serial, secs: float) -> bytes:
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
        else:
            time.sleep(0.02)
    return buf


def wait_for(ser: serial.Serial, needles, timeout: float) -> tuple[bytes, bool]:
    if isinstance(needles, (bytes, bytearray)):
        needles = [needles]
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            for nd in needles:
                if nd in buf:
                    return buf, True
        else:
            time.sleep(0.02)
    return buf, False


def sh(cmd: list[str] | str) -> int:
    print("SH:", cmd if isinstance(cmd, str) else " ".join(cmd), flush=True)
    if isinstance(cmd, str):
        return subprocess.call(cmd, shell=True)
    return subprocess.call(cmd)


def chips_alive_host() -> bool:
    try:
        out = subprocess.check_output(
            ["lynx-showinfo", "-f"], stderr=subprocess.STDOUT, text=True, errors="replace")
    except Exception:
        return False
    plain = re.sub(r"\x1b\[[0-9;]*m", "", out)
    m = re.search(r"\[2\].*?ALIVE:([0-9A-Fa-f]+)", plain)
    if not m:
        return False
    try:
        return int(m.group(1), 16) != 0
    except ValueError:
        return False


class CliKeepalive:
    def __init__(self, ser: serial.Serial, interval_s: float = CLI_KEEPALIVE_S):
        self.ser = ser
        self.interval_s = interval_s
        self._stop = threading.Event()
        self._thr: threading.Thread | None = None
        self.ticks = 0

    def start(self) -> None:
        self._stop.clear()
        self._thr = threading.Thread(target=self._run, name="mcu-cli-ka", daemon=True)
        self._thr.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thr:
            self._thr.join(timeout=2.0)
            self._thr = None

    def _run(self) -> None:
        # NUL resets idle timeout without completing a command line
        while not self._stop.wait(self.interval_s):
            try:
                self.ser.write(b"\x00")
                self.ser.flush()
                self.ticks += 1
            except Exception as e:
                print("[cli-ka] write fail:", e, flush=True)
                break


def teardown_quit_wait_ka_reset(sm: serial.Serial | None, wait_s: float) -> None:
    print("## teardown: quit CLI", flush=True)
    if sm is not None and getattr(sm, "is_open", False):
        try:
            sm.write(b"quit\r\n")
            sm.flush()
            q, _ = wait_for(sm, [b"exit", b"timeout", b"ready"], 5)
            print(ascii(q[-200:]), flush=True)
            drain(sm, 0.5)
        except Exception as e:
            print("[teardown] quit:", e, flush=True)

    print("## teardown: wait %.0fs after quit (MCU main loop resume)" % wait_s, flush=True)
    time.sleep(wait_s)

    print("## teardown: KA200 reset", flush=True)
    sh(["lynx-showinfo", "-r", "-l", "0"])


# ---------------------------------------------------------------------------
# Phase 1–2: KA reset / boot — never send +++ here
# ---------------------------------------------------------------------------

def wait_ka_stable_after_reset(ska: serial.Serial, sm: serial.Serial,
                               boot_timeout: float = 90.0,
                               settle_s: float = PRE_CLI_SETTLE_S) -> bool:
    """
    After a KA / link reset:
      - watch MCU for optional 'soc rst' (do NOT enter CLI)
      - then wait KA boot marks + host chip ALIVE + msh poke
      - quiet settle; if late soc rst, recurse
    """
    print("## phase: wait KA stable (no MCU CLI)", flush=True)

    end_rst = time.time() + 25.0
    mcu_buf = b""
    ka_buf = b""
    saw_soc_rst = False
    while time.time() < end_rst:
        if sm.in_waiting:
            chunk = sm.read(sm.in_waiting)
            mcu_buf += chunk
            for m in MCU_RST_MARKS:
                if m in mcu_buf:
                    saw_soc_rst = True
                    print("[mcu] saw %r — KA will reboot; still NOT entering CLI" % m,
                          flush=True)
                    ka_buf = b""
                    ska.reset_input_buffer()
                    break
            if saw_soc_rst:
                break
        if ska.in_waiting:
            ka_buf += ska.read(ska.in_waiting)
            if any(n in ka_buf for n in KA_BOOT_MARKS):
                print("[ka] boot mark during rst-wait", flush=True)
                break
        time.sleep(0.05)

    print("[mcu] rst-window:", ascii(mcu_buf[-250:]), "saw_soc_rst=", saw_soc_rst, flush=True)

    print("[ka] waiting boot marks (timeout %.0fs)..." % boot_timeout, flush=True)
    end = time.time() + boot_timeout
    last_poke = 0.0
    while time.time() < end:
        if sm.in_waiting:
            chunk = sm.read(sm.in_waiting)
            if any(m in chunk for m in MCU_RST_MARKS):
                print("[mcu] another soc rst during boot wait — restart KA wait",
                      flush=True)
                ka_buf = b""
                ska.reset_input_buffer()
                end = time.time() + boot_timeout
                continue
        if ska.in_waiting:
            ka_buf += ska.read(ska.in_waiting)
            if any(n in ka_buf for n in KA_BOOT_MARKS):
                print("[ka] boot OK:\n", ascii(ka_buf[-400:]), flush=True)
                break
        now = time.time()
        if now - last_poke >= 5.0:
            try:
                ska.write(b"\r")
            except Exception:
                pass
            last_poke = now
        time.sleep(0.05)
    else:
        print("[ka] boot marks timeout; last:\n", ascii(ka_buf[-400:]), flush=True)
        if not chips_alive_host():
            return False
        print("[host] chips ALIVE despite no UART mark — try msh poke", flush=True)

    for _ in range(20):
        if chips_alive_host():
            break
        time.sleep(1)
    else:
        print("[host] Board2 chips not ALIVE", flush=True)
        return False
    print("[host] Board2 chips ALIVE", flush=True)

    ska.write(b"\r\n")
    ska.flush()
    kb, ok = wait_for(ska, [b"msh >"], 8)
    print("[ka] msh poke:", ascii(kb[-200:]), "ok=", ok, flush=True)
    if not ok:
        return False

    print("[settle] %.0fs quiet (abort if soc rst) before i2c/CLI" % settle_s, flush=True)
    late, had = wait_for(sm, list(MCU_RST_MARKS), settle_s)
    if had:
        print("[mcu] late soc rst during settle — recursive wait:\n",
              ascii(late[-200:]), flush=True)
        return wait_ka_stable_after_reset(ska, sm, boot_timeout, settle_s)

    print("[phase] KA stable — I2C should be up (auto); CLI next", flush=True)
    return True


def reset_and_wait_ka(ska: serial.Serial, sm: serial.Serial,
                      settle_s: float = PRE_CLI_SETTLE_S) -> bool:
    print("## phase: KA200 reset (MCU stays out of CLI)", flush=True)
    ska.reset_input_buffer()
    sm.reset_input_buffer()
    sh(["lynx-showinfo", "-r", "-l", "0"])
    return wait_ka_stable_after_reset(ska, sm, settle_s=settle_s)


def ensure_ka_ready(ska: serial.Serial, sm: serial.Serial, do_reset: bool,
                    settle_s: float = PRE_CLI_SETTLE_S) -> bool:
    if do_reset:
        return reset_and_wait_ka(ska, sm, settle_s=settle_s)

    print("## phase: use current KA (no reset)", flush=True)
    if not chips_alive_host():
        print("[host] not ALIVE — forcing reset path", flush=True)
        return reset_and_wait_ka(ska, sm, settle_s=settle_s)

    ska.write(b"\r\n")
    ska.flush()
    kb, ok = wait_for(ska, [b"msh >"], 6)
    print(ascii(kb[-200:]), "ok=", ok, flush=True)
    if not ok:
        return reset_and_wait_ka(ska, sm, settle_s=settle_s)

    print("[settle] %.0fs before CLI" % settle_s, flush=True)
    late, had = wait_for(sm, list(MCU_RST_MARKS), settle_s)
    if had:
        print("[mcu] soc rst while already-up — wait reboot", flush=True)
        return wait_ka_stable_after_reset(ska, sm, settle_s=settle_s)
    return True


def enter_mcu_cli(sm: serial.Serial) -> bool:
    print("## phase: MCU enter CLI (KA already stable)", flush=True)
    drain(sm, 0.5)
    mb, _ = wait_for(sm, [b"UART2 CLI ready", b"type +++"], 3)
    if mb:
        print("[mcu]", ascii(mb[-150:]), flush=True)

    sm.write(b"+++")
    sm.flush()
    me, ok = wait_for(sm, [b"[MCU CMD]", b"enter CLI"], 8)
    print(ascii(me[-300:]), flush=True)
    if not ok:
        return False
    if any(m in me for m in MCU_RST_MARKS):
        print("FAIL: soc rst appeared while entering CLI — sequence wrong", flush=True)
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="MCU CLI I2C e2e (CLI only after KA stable)")
    ap.add_argument("--skip-upgrade", action="store_true")
    ap.add_argument(
        "--no-reset",
        action="store_true",
        help="skip lynx-showinfo -r if KA already up (still settles / watches soc rst)",
    )
    ap.add_argument(
        "--fw",
        default="/home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_20260715_v5.0.bin",
    )
    ap.add_argument("--soc", type=int, default=SOC)
    ap.add_argument("--post-quit-wait", type=float, default=POST_QUIT_WAIT_S)
    ap.add_argument("--pre-cli-settle", type=float, default=PRE_CLI_SETTLE_S)
    args = ap.parse_args()
    pre_cli_settle = args.pre_cli_settle
    post_quit_wait = args.post_quit_wait

    subprocess.call("fuser -k %s %s 2>/dev/null" % (KA_DEV, MCU_DEV), shell=True)
    time.sleep(0.5)

    if not args.skip_upgrade:
        sh(["lynx-showinfo", "-r", "-l", "0"])
        time.sleep(12)
        sh([
            "/usr/local/lynx/tools/ka200_tools",
            "-u", args.fw, "-l", "0", "-i", "2", "-k", "30",
        ])

    ska = serial.Serial(KA_DEV, BAUD, timeout=0.05)
    sm = serial.Serial(MCU_DEV, BAUD, timeout=0.05)
    ska.reset_input_buffer()
    sm.reset_input_buffer()

    rc = 0
    keepalive: CliKeepalive | None = None
    cli_entered = False

    try:
        if not ensure_ka_ready(ska, sm, do_reset=not args.no_reset,
                               settle_s=pre_cli_settle):
            print("FAIL: KA not stable")
            rc = 1
            return rc

        drain(ska, 0.3)
        print("## phase: confirm KA I2C auto-start", flush=True)
        # Boot already started I2C (BSP_I2C_DEFER off). Look for ready in recent log
        # or just ensure msh still responds before entering MCU CLI.
        ska.write(b"\r\n")
        ska.flush()
        ir, ok = wait_for(ska, [b"msh >"], 5)
        print(ascii(ir[-300:]), flush=True)
        if not ok:
            print("FAIL: KA msh not responding (I2C auto-start boot?)")
            rc = 2
            return rc

        late, had = wait_for(sm, list(MCU_RST_MARKS), 2.0)
        if had:
            print("FAIL: soc rst after KA ready — do not enter CLI; re-run", flush=True)
            rc = 5
            return rc

        if not enter_mcu_cli(sm):
            print("FAIL: enter CLI")
            rc = 3
            return rc
        cli_entered = True

        keepalive = CliKeepalive(sm)
        keepalive.start()
        print("[cli-ka] every %.1fs" % CLI_KEEPALIVE_S, flush=True)

        print("## i2c_scan", flush=True)
        sm.write(b"i2c_scan\r\n")
        sm.flush()
        mr, _ = wait_for(sm, [b"found ", b"aborted", b"bus fault"], 30)
        print(ascii(mr[-900:]), flush=True)

        time.sleep(0.3)
        ska.write(b"\r\n")
        ska.flush()
        st, _ = wait_for(ska, [b"msh >"], 8)
        print("KA after scan:\n", ascii(st[-350:]), flush=True)
        if b"msh >" not in st:
            print("FAIL: KA msh dead after scan")
            rc = 4
            return rc

        soc = args.soc
        print("## ka200 reg read soc=%u" % soc, flush=True)
        sm.write(("ka200 reg read %u %s 4\r\n" % (soc, REG_ADDR_RD)).encode())
        sm.flush()
        rr, _ = wait_for(sm, [b" ok:", b"reg read fail", b"fail"], 12)
        print(ascii(rr[-500:]), flush=True)

        print("## ka200 reg write/read scratch soc=%u" % soc, flush=True)
        sm.write(("ka200 reg write %u %s 0xA5 0x5A 0x12 0x34\r\n" % (
            soc, REG_ADDR_WR)).encode())
        sm.flush()
        wr, _ = wait_for(sm, [b"ok", b"fail", b"[MCU CMD]"], 12)
        print("write:", ascii(wr[-400:]), flush=True)

        sm.write(("ka200 reg read %u %s 4\r\n" % (soc, REG_ADDR_WR)).encode())
        sm.flush()
        rb, _ = wait_for(sm, [b" ok:", b"fail"], 12)
        print("readback:", ascii(rb[-400:]), flush=True)

    finally:
        if keepalive is not None:
            keepalive.stop()
            print("[cli-ka] stopped ticks=%d" % keepalive.ticks, flush=True)
        if cli_entered:
            teardown_quit_wait_ka_reset(sm, post_quit_wait)
        else:
            print("## teardown: skipped quit (CLI not entered); KA reset only", flush=True)
            sh(["lynx-showinfo", "-r", "-l", "0"])
        try:
            ska.close()
        except Exception:
            pass
        try:
            if sm is not None and getattr(sm, "is_open", False):
                sm.close()
        except Exception:
            pass

    print("==== DONE rc=%d ====" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
