#!/usr/bin/env python3
"""
xmodem boot RT-Thread (lynx-showinfo -r -l 0), wait for emmc_biz heartbeat,
verify lynx-showinfo topology (Link/Board ALIVE), then ka200_tools -u upgrade.
Keeps one serial session open so [biz][upgrade] UART logs are captured.

Run on test host (58.36):
  sudo python3 flash_host_upgrade_run.py \\
      --ka200 /home/lynxi/xia/ka200/HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin -l 0 -i 2 -k 30
"""

import argparse
import os
import re
import subprocess
import sys
import time

from flash_common import check_link_topology, flash_images, xmodem_dir

KA200_TOOLS = os.environ.get("HP232X_KA200_TOOLS", "/usr/local/lynx/tools/ka200_tools")
BOOT_WAIT = int(os.environ.get("HP232X_BOOT_WAIT", "120"))
UPGRADE_WAIT = int(os.environ.get("HP232X_UPGRADE_WAIT", "300"))


def read_uart(ser, logf, deadline):
    n = 0
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            logf.write(data)
            logf.flush()
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
            n += len(data)
        else:
            time.sleep(0.01)
    return n


def wait_pattern(log_path, pattern, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        with open(log_path, "rb") as f:
            text = f.read().decode(errors="replace")
        if re.search(pattern, text, re.I):
            return True
        time.sleep(0.5)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ka200", required=True, help="host upgrade firmware path")
    ap.add_argument("-l", type=int, default=0)
    ap.add_argument("-i", type=int, default=2)
    ap.add_argument("-k", type=int, default=30)
    ap.add_argument("--log", default="/tmp/hp232x_host_upgrade.log")
    ap.add_argument("--skip-flash", action="store_true")
    args = ap.parse_args()

    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    with open(args.log, "wb") as logf:
        if not args.skip_flash:
            xdir = xmodem_dir()
            # reset: lynx-showinfo -r -l 0; topology gate before ka200_tools uses lynx-showinfo -l 0
            ser = flash_images(xdir)
            print("[upgrade] waiting for emmc_biz heartbeat (max %ds)..." % BOOT_WAIT,
                  flush=True)
            hb_deadline = time.time() + BOOT_WAIT
            got_hb = False
            while time.time() < hb_deadline:
                data = ser.read(4096)
                if data:
                    logf.write(data)
                    logf.flush()
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
                    chunk = data.decode(errors="replace")
                    if re.search(r"Heart-beat reported|Entering main task|emmc_biz entry",
                                 chunk, re.I):
                        got_hb = True
                        print("[upgrade] heartbeat OK", flush=True)
                        break
                else:
                    time.sleep(0.01)
            if not got_hb:
                print("[upgrade] ERROR: emmc_biz not ready — abort", flush=True)
                ser.close()
                return 1
        else:
            import serial
            ser = serial.Serial(os.environ.get("HP232X_SERIAL", "/dev/ttyUSB0"),
                                115200, timeout=0.05)
            print("[upgrade] skip-flash: assume KA200 already running", flush=True)
            read_uart(ser, logf, time.time() + 2)

        if not check_link_topology(args.l, args.i, args.k):
            print("[upgrade] ERROR: topology check failed — abort host upgrade",
                  flush=True)
            ser.close()
            return 1

        print("[upgrade] prerequisites OK, starting ka200_tools...", flush=True)
        cmd = [KA200_TOOLS, "-u", args.ka200,
               "-l", str(args.l), "-i", str(args.i), "-k", str(args.k), "-f"]
        print("[upgrade] running: %s" % " ".join(cmd), flush=True)
        proc = subprocess.Popen(cmd)
        done = False
        tools_done = False
        end_time = time.time() + UPGRADE_WAIT
        post_until = 0.0
        while not done and (time.time() < end_time or time.time() < post_until):
            data = ser.read(4096)
            if data:
                logf.write(data)
                logf.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
                chunk = data.decode(errors="replace")
                if "OK FlashWrite" in chunk or "[biz][upgrade] FAIL" in chunk:
                    done = True
                    break
            if not tools_done and proc.poll() is not None:
                tools_done = True
                post_until = time.time() + 45
            if not data:
                time.sleep(0.01)

        rc_tools = proc.poll()
        if rc_tools is None:
            rc_tools = proc.wait(timeout=10)

        # Verify flash content via msh (read-only, not part of upgrade cmd path).
        ser.write(b"flash read 0xa6000 64\n")
        verify_until = time.time() + 8
        while time.time() < verify_until:
            data = ser.read(4096)
            if data:
                logf.write(data)
                logf.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
            else:
                time.sleep(0.05)
        ser.close()

    size = os.path.getsize(args.log)
    print("[upgrade] log saved: %s (%d bytes)" % (args.log, size), flush=True)
    print("KA200_TOOLS_RC=%d" % rc_tools, flush=True)
    print("UART_BYTES=%d" % size, flush=True)

    with open(args.log, "rb") as f:
        text = f.read().decode(errors="replace")
    ok = (rc_tools == 0
          and "topology OK" in text
          and "Send Update Command/Firmware Successfully" in text
          and "[biz][upgrade] FAIL" not in text
          and "[biz][upgrade] OK FlashWrite" in text
          and "SError" not in text)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
