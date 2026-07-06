#!/usr/bin/env python3

import argparse
import os
import struct
import subprocess
import sys


MAGIC = 0x4C584B4A
FLAG = 0x4A554D50
HEADER_SIZE = 0x20
VERSION = 0x01


def analyze_elf_sections(elf_path):
    """分析 ELF 文件的各段占用情况"""

    # 使用 objdump -h 获取段信息
    objdump = "/work/tools/cross-compiler/gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-objdump"

    try:
        result = subprocess.run([objdump, "-h", elf_path],
                                capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Warning: Failed to analyze ELF sections: {e}")
        return None
    except FileNotFoundError:
        print(f"Warning: objdump tool not found at {objdump}")
        return None

    sections = {}
    for line in result.stdout.split('\n'):
        # Parse lines like: "  0 .text         0002d110  0000000004000800  ..."
        parts = line.strip().split()
        if len(parts) >= 6 and parts[0].isdigit():
            idx = int(parts[0])
            name = parts[1]
            size = int(parts[2], 16)
            vma = int(parts[3], 16)
            lma = int(parts[4], 16)

            # Filter relevant sections
            if name in ['.head', '.text', '.early_hp232x', '.eh_frame', '.data',
                       '.bss.noclean.mmu_table', '.mmu_table', '.bss', '.rodata', '.stack']:
                sections[name] = {
                    'idx': idx,
                    'size': size,
                    'vma': vma,
                    'lma': lma,
                    'size_kb': size / 1024
                }

    return sections


def print_section_analysis(sections):
    """打印段分析结果（精简表格）"""

    if not sections:
        return

    # IRAM0 段列表
    iram0_sections = []
    iram0_total = 0

    for name in ['.head', '.text', '.early_hp232x', '.data']:
        if name in sections:
            sec = sections[name]
            # Adjust VMA display: kernel linked at 0x04040000+, subtract offset for display
            display_addr = sec['vma'] if sec['vma'] >= 0x4000000 else sec['vma']
            iram0_sections.append((name, display_addr, sec['size_kb']))
            iram0_total += sec['size']

    # IRAM1: full .bss plus optional mmu_table sub-section
    iram1_bss = 0
    iram1_mmu = 0
    bss_addr = 0
    mmu_addr = 0
    if '.bss' in sections:
        iram1_bss = sections['.bss']['size']
        bss_addr = sections['.bss']['vma']
    for name in ['.bss.noclean.mmu_table', '.mmu_table']:
        if name in sections:
            iram1_mmu = sections[name]['size']
            mmu_addr = sections[name]['vma']
            break

    # 打印精简表格
    print("\nSection Analysis:")
    print("-" * 60)
    print(f"{'Section':<20} {'Address':>12} {'Size (KB)':>10}")
    print("-" * 60)

    # IRAM0 sections
    for name, addr, size_kb in sorted(iram0_sections, key=lambda x: x[1]):
        print(f"{name:<20} 0x{addr:08X}  {size_kb:>10.2f}")

    # Separator
    print("-" * 60)

    # IRAM1 BSS
    if iram1_bss:
        print(f".bss (IRAM1)         0x{bss_addr:016X}  {iram1_bss/1024:>10.2f}")
    if iram1_mmu:
        print(f"  mmu_table (noclean) 0x{mmu_addr:016X}  {iram1_mmu/1024:>10.2f}")

    print("-" * 60)

    # Summary
    total_file = iram0_total / 1024
    total_bss = iram1_bss / 1024

    print(f"{'IRAM0 (File)':<20} {'':12}  {total_file:>10.2f}")
    print(f"{'IRAM1 (BSS)':<20} {'':12}  {total_bss:>10.2f}")
    print(f"{'Total Memory':<20} {'':12}  {(total_file + total_bss):>10.2f}")

    # Constraint check
    iram0_ok = iram0_total <= 256 * 1024
    iram1_ok = iram1_bss <= 256 * 1024 and bss_addr >= 0x100040000

    print("-" * 60)
    if iram0_ok and iram1_ok:
        print(f"✅ All memory constraints satisfied (IRAM0≤256KB, IRAM1≤256KB)")
    else:
        print(f"❌ Memory constraint violation detected!")
    print()


def main():
    parser = argparse.ArgumentParser(description="Generate hp232x boot image with 32-byte header (BL1-bootrom-compatible)")
    parser.add_argument("input", help="input raw binary (linked from 0x04000020 or 0x04040020)")
    parser.add_argument("output", help="output image file")
    parser.add_argument("--dest-addr", type=lambda x: int(x, 0), default=0x04000020,
                       help="destination address (default: 0x04000020, BL1_BOOT mode)")
    parser.add_argument("--next-offset", type=lambda x: int(x, 0), default=0x20000)
    parser.add_argument("--headersize", type=lambda x: int(x, 0), default=0x20)
    parser.add_argument("--end-flag", type=lambda x: int(x, 0), default=0x0)  # Not used in BL1, preserved
    parser.add_argument("--version", type=lambda x: int(x, 0), default=0x1)
    parser.add_argument("--elf", help="ELF file for section analysis (optional)")
    args = parser.parse_args()

    # 分析 ELF 段（如果提供了 ELF 文件）
    if args.elf and os.path.exists(args.elf):
        sections = analyze_elf_sections(args.elf)
        if sections:
            print_section_analysis(sections)

    # 打包二进制文件
    with open(args.input, "rb") as src:
        payload = src.read()

    file_size = len(payload)

    print(f"【Generating Boot Image】")
    print(f"  Input:       {args.input}")
    print(f"  Output:      {args.output}")
    print(f"  Payload:     {file_size} bytes ({file_size / 1024:.2f} KB)")
    print(f"  Dest Addr:   0x{args.dest_addr:08X}")
    print(f"  Next Offset: 0x{args.next_offset:X}")
    print(f"  Header Size: {args.headersize} bytes")

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
        args.headersize,          # 0x1C-1F: BL1 reads headersize from here
    )

    # Validate header size
    actual_header_size = len(layout)
    if actual_header_size not in [32, 36]:  # Allow 36B for alignment variant
        print(f"Error (verify): Header size is {actual_header_size}, expected 32-36 bytes", file=sys.stderr)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as dst:
        dst.write(layout)
        dst.write(payload)

    total_size = len(layout) + len(payload)
    print(f"  Total Size:  {total_size} bytes ({total_size / 1024:.2f} KB)")
    print(f"✅ Boot image generated successfully!\n")


if __name__ == "__main__":
    main()