#!/usr/bin/env python3
"""xmodem flash + 120s boot log (generic). Run on 58.36: sudo python3 flash_and_log.py"""

import os
import sys
import time

from flash_common import run_flash_and_log

LOG = os.environ.get(
    "HP232X_LOG",
    "/tmp/hp232x_flash_%d.log" % int(time.time()),
)


def main():
    path = run_flash_and_log(LOG)
    print("\n=== grep hints ===")
    print("grep -aE 'SMP|PMON|emmc|heart|fail|error|msh' %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
