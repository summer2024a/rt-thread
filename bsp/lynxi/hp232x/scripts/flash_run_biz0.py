#!/usr/bin/env python3
"""eMMC / biz test: flash + log, grep for heartbeat. Run on 58.36."""

import os
import re
import sys
import time

from flash_common import run_flash_and_log

LOG = os.environ.get(
    "HP232X_LOG",
    "/tmp/hp232x_biz0_%d.log" % int(time.time()),
)

BIZ_PATTERNS = [
    (r"\[board\] IRAM1 layout|\[board\] STEP1", "RT-Thread board init"),
    (r"tuning OK tap=", "eMMC tuning OK"),
    (r"Heart-beat reported|heart-beat sent|heartbeat", "heartbeat"),
    (r"Entering main task|emmc_biz entry", "biz main loop"),
    (r"HS400|HS200", "eMMC speed mode"),
]

# Early-boot only: firmware hung before board init (often IRAM1 BSS overflow)
EARLY_BOOT_ONLY = re.compile(
    r"(P2I0|IBKSUE|BOOT)", re.I)
BOARD_INIT = re.compile(
    r"\[board\] IRAM1 layout|\[board\] STEP1", re.I)


def analyze(log_path):
    with open(log_path, "rb") as f:
        text = f.read().decode(errors="replace")
    print("\n=== biz analysis ===")
    if (EARLY_BOOT_ONLY.search(text) and not BOARD_INIT.search(text)
            and len(text) < 8192):
        print("  [FAIL] early boot only (P2I0/BOOT, no board init)")
        print("         hint: check IRAM1 BSS overflow (__bss_end in rtthread.map)")
        print("         see BIZ_PORTING.md §4.5 / §8")
        return False
    ok = True
    for pat, label in BIZ_PATTERNS:
        if re.search(pat, text, re.I):
            print("  [PASS] %s" % label)
        else:
            print("  [----] %s (not found)" % label)
            if label in ("heartbeat",):
                ok = False
    return ok


def main():
    path = run_flash_and_log(LOG)
    ok = analyze(path)
    print("\nlog: %s" % path)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
