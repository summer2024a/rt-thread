#!/usr/bin/env python3
"""SMP / Core1 test: flash + log, grep for PMON CPU1 PASS. Run on 58.36."""

import os
import re
import sys
import time

from flash_common import run_flash_and_log

LOG = os.environ.get(
    "HP232X_LOG",
    "/tmp/hp232x_smp_%d.log" % int(time.time()),
)

SMP_PATTERNS = [
    (r"\[SMP\] CPU1 ready", "SMP CPU1 idle ready"),
    (r"\[PMON\]\[CPU1\] PASS", "PMON CPU1 bind PASS"),
    (r"\[PMON\]\[CPU1\].*tick=.*\(\+50\)", "PMON tick +50/500ms"),
    (r"\[SMP\] bind test PASS", "SMP bind self-test PASS"),
    (r"-->rt_hw_gtimer_init ok", "arch timer init"),
    (r"msh >", "msh shell"),
]


def analyze(log_path):
    with open(log_path, "rb") as f:
        text = f.read().decode(errors="replace")
    print("\n=== SMP analysis ===")
    ok = True
    for pat, label in SMP_PATTERNS:
        m = re.search(pat, text)
        if m:
            print("  [PASS] %s" % label)
        elif label in ("SMP bind self-test PASS",):
            print("  [SKIP] %s (enable BSP_USING_HP232X_SMP_BIND_TEST)" % label)
        else:
            print("  [FAIL] %s" % label)
            ok = False
    if re.search(r"\[PMON\]\[CPU1\] FAIL|CPU1 NOT online|bootstrap timeout", text):
        ok = False
        print("  [FAIL] CPU1 error marker in log")
    return ok


def main():
    path = run_flash_and_log(LOG)
    ok = analyze(path)
    print("\nlog: %s" % path)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
