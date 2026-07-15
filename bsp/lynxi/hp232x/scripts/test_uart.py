#!/usr/bin/env python3
"""xmodem flash only (no boot log). Run on 58.36 after reset + .U."""

import os
import sys

from flash_common import flash_images, xmodem_dir

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def main():
    os.chdir(SCRIPT_DIR)
    ser = flash_images(xmodem_dir())
    ser.close()
    print("[flash] done (no log capture — use flash_and_log.py or flash_run_smp.py)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
