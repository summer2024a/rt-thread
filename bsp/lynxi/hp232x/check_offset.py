#!/usr/bin/env python3
import sys

AARCH64_INSTR = {
    0xd28004f1: "mov x21, xzr",
    0xd28006f2: "mov x22, xzr",
    0xd28008f3: "mov x23, xzr",
    0xd2800af4: "mov x24, xzr",
    0xd53b420a: "mrs x10, mpidr_el1",
    0xd2800019: "mov x9, #0xffffffff",
    0xfa0f016a: "tst x10, x9",
    0x54000061: "b.ne .secondary_spin",
    0xd2800329: "mov x9, #0x12500000",
    0xb940082a: "ldr w10, [x9, #0x00]",
}

def read_u32_le(data, offset):
    return int.from_bytes(data[offset:offset+4], 'little')

def main():
    if len(sys.argv) < 2:
        print("Usage: check_offset.py bin_file [offset]")
        exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    offset = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x20

    print(f"Checking {sys.argv[1]} from offset 0x{offset:x}:")
    for i in range(16):
        addr = offset + i * 4
        instr = read_u32_le(data, addr)
        decoded = AARCH64_INSTR.get(instr, f"??? 0x{instr:08x}")
        print(f"  0x{addr:08x}: 0x{instr:08x}   {decoded}")

if __name__ == "__main__":
    main()