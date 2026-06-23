#!/usr/bin/env python3

import argparse
import os
import struct


MAGIC = 0x4C584B4A
FLAG = 0x4A554D50
HEADER_SIZE = 0x20
VERSION = 0x01


def main():
    parser = argparse.ArgumentParser(description="Generate hp232x boot image with 32-byte header (BL1-bootrom-compatible)")
    parser.add_argument("input", help="input raw binary (linked from 0x04000020)")
    parser.add_argument("output", help="output image file")
    parser.add_argument("--dest-addr", type=lambda x: int(x, 0), default=0x04000020,
                       help="destination address (default: 0x04000020)")
    parser.add_argument("--next-offset", type=lambda x: int(x, 0), default=0x20000)
    parser.add_argument("--headersize", type=lambda x: int(x, 0), default=0x20)
    parser.add_argument("--end-flag", type=lambda x: int(x, 0), default=0x0)  # Not used in BL1, preserved
    parser.add_argument("--version", type=lambda x: int(x, 0), default=0x1)
    args = parser.parse_args()

    with open(args.input, "rb") as src:
        payload = src.read()

    file_size = len(payload)
    
    # BL1 bootrom final jump logic (bl1_entrypoint.S line 185-187):
    #   bl boot_mem_select  # x2 = 0x04000000
    #   ISB
    #   ldrh w1, [x2, #0x1c]  # READ HEADERSIZE FROM OFFSET 0x1C
    #   add x0, x2, w1        # jump_addr = IRAM + headersize
    #   br x0
    # BL1 expects headersize at offset 0x1C exactly
    # Use simple 32B layout: I(4)+I(4)+I(4)+I(4)+I(4)+I(4)+I(4)+I(4) = 32 bytes
    layout = struct.pack(
        "<IIIIIIII",
        MAGIC,                    # 0x00-03
        FLAG,                     # 0x04-07
        args.dest_addr & 0xFFFFFFFF,  # 0x08-0B: dest_addr low (unused by BL1)
        0x0,                      # 0x0C-0F: dest_addr high
        file_size,                # 0x10-13: file_size (unused by BL1)
        args.next_offset,         # 0x14-17: next_offset (unused by BL1)
        0x0,                      # 0x18-1B: padding
        args.headersize,          # 0x1C-1F: ⚠️ BL1 reads headersize from here
    )

    # Validate header size
    actual_header_size = len(layout)
    if actual_header_size not in [32, 36]:  # Allow 36B for alignment variant
        print(f"Error (verify): Header size is {actual_header_size}, expected 32-36 bytes", file=sys.stderr)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as dst:
        dst.write(layout)
        dst.write(payload)


if __name__ == "__main__":
    main()