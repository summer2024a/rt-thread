#!/usr/bin/env python3
"""
Pack rtthread.bin into Host upgrade image for ka200_tools -u (old hp640 FlashWrite).

Flash layout after ka200_tools repack + FlashWrite @0xA6000:

  @0xA6000  Head640{size, md5} + pad to 4KB   (written by ka200_tools)
  @0xA7000  THIS FILE as body                  (copied by hp640_bl to 0x4040000)

hp640_bl jumper then jumps to dst+0x20 = 0x4040020. Therefore the payload
(rtthread.bin, linked @0x04040020) MUST start at file offset 0x20.

Header is exactly 0x20 bytes (JKXL magic kept for ka200_tools), not 0x40 —
a 0x40 header left zeros at +0x20 and cold-boot hung after memcpy.

Default output name:
  HP232x_KA200_Serdes_Update_YYYYMMDD_vMAJOR.MINOR.bin

Usage (JKXL Host image — unchanged):
    python3 mk_ka200_image.py rtthread.bin
    python3 mk_ka200_image.py rtthread.bin [output.bin]
    python3 mk_ka200_image.py rtthread.bin --version 0x0409

Usage (Head640 — same as ka200_tools repack_firmware):
    python3 mk_ka200_image.py --head640 HP232x_KA200_Serdes_Update_....bin
    python3 mk_ka200_image.py --head640 in.bin out_Head640.bin
"""

import argparse
import datetime
import hashlib
import os
import struct
import sys

MAGIC = 0x4C584B4A      # JKXL — required by ka200_tools
FLAG = 0x4A554D50       # PMUJ
END_FLAG = 0x454E4421   # !END
# Must be 0x20: jumper entry = body+0x20; payload must begin there.
HEADER_SIZE = 0x20
DEFAULT_DEST = 0x04020000
DEFAULT_NEXT = 0x20020
VERSION = 0x0500        # packed into header; filename → v5.0

NAME_PREFIX = "HP232x_KA200_Serdes_Update"
ALIGN_4K = 4096
ALIGN_512 = 512
MAX_HEAD640 = 256 * 1024


def version_tag(version):
    """Header version → 'vMAJOR.MINOR' (same style as hp640 addheader 0x040b → 4.11)."""
    return "v%d.%d" % ((version >> 8) & 0xFF, version & 0xFF)


def default_ka200_basename(version=VERSION, date_str=None):
    if not date_str:
        date_str = datetime.date.today().strftime("%Y%m%d")
    return "%s_%s_%s.bin" % (NAME_PREFIX, date_str, version_tag(version))


def default_ka200_path(outdir=".", version=VERSION, date_str=None):
    return os.path.join(outdir, default_ka200_basename(version, date_str))


def default_head640_path(body_path):
    base, ext = os.path.splitext(os.path.abspath(body_path))
    if not ext:
        ext = ".bin"
    return base + "_Head640" + ext


def pack_ka200(payload_path, out_path, dest_addr, next_offset, version):
    payload_size = os.path.getsize(payload_path)
    if payload_size > 256 * 1024 - HEADER_SIZE:
        print("ERROR: payload %d bytes exceeds 256KB ka200 limit" % payload_size,
              file=sys.stderr)
        return 1

    # Fixed little-endian layout (no platform 'L'): exactly HEADER_SIZE bytes.
    # magic, flag, dest_lo, dest_hi, payload_size, next_offset, end, hdrsz, ver
    header = struct.pack(
        "<IIIIIIIHH",
        MAGIC,
        FLAG,
        dest_addr & 0xFFFFFFFF,
        (dest_addr >> 32) & 0xFFFFFFFF,
        payload_size,
        next_offset & 0xFFFFFFFF,
        END_FLAG,
        HEADER_SIZE,
        version & 0xFFFF,
    )
    if len(header) != HEADER_SIZE:
        print("ERROR: header pack len %d != %d" % (len(header), HEADER_SIZE),
              file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(out_path))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(payload_path, "rb") as fin, open(out_path, "wb") as fout:
        fout.write(header)
        while True:
            chunk = fin.read(65536)
            if not chunk:
                break
            fout.write(chunk)

    total = os.path.getsize(out_path)
    print("ka200 image: %s" % out_path)
    print("  payload:   %d bytes (starts at file offset 0x%X for jumper +0x20)" %
          (payload_size, HEADER_SIZE))
    print("  total:     %d bytes (Head640.size after ka200_tools repack)" % total)
    print("  dest:      0x%08X  next: 0x%X  version: %s  hdr: 0x%X" % (
        dest_addr, next_offset, version_tag(version), HEADER_SIZE))
    return 0


def pack_head640(body_path, out_path):
    """
    Match ka200_tools repack_firmware():
      size(4 LE) + MD5(16 of whole body) + pad to 4KB + body + pad to 512.

    body must start with JKXL (0x4C584B4A). Final size ≤ 256KB and 512-aligned
    so FlashWrite @0xA6000 accepts it.
    """
    with open(body_path, "rb") as f:
        body = f.read()

    file_size = len(body)
    if file_size < 4:
        print("ERROR: empty/short body: %s" % body_path, file=sys.stderr)
        return 1

    magic = struct.unpack("<I", body[:4])[0]
    if magic != MAGIC:
        print("ERROR: body magic 0x%08X != JKXL (0x%08X); "
              "pass Serdes_Update / ka200 JKXL image" % (magic, MAGIC),
              file=sys.stderr)
        return 1

    md5 = hashlib.md5(body).digest()
    hdr = struct.pack("<I", file_size) + md5
    cur = len(hdr)
    pad4k = (ALIGN_4K - (cur % ALIGN_4K)) % ALIGN_4K
    total = cur + pad4k + file_size
    pad512 = (ALIGN_512 - (total % ALIGN_512)) % ALIGN_512
    total += pad512

    if total > MAX_HEAD640:
        print("ERROR: Head640 total %d exceeds 256KB" % total, file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(out_path))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(out_path, "wb") as fout:
        fout.write(hdr)
        if pad4k:
            fout.write(b"\x00" * pad4k)
        fout.write(body)
        if pad512:
            fout.write(b"\x00" * pad512)

    print("Head640 image: %s" % out_path)
    print("  body:      %s (%d bytes, JKXL)" % (body_path, file_size))
    print("  MD5:       %s" % md5.hex())
    print("  layout:    size+md5 + pad4K → body@0x%X + pad512" % ALIGN_4K)
    print("  total:     %d bytes (FlashWrite @0xA6000, body @0xA7000)" % total)
    return 0


def main():
    p = argparse.ArgumentParser(
        description="Pack RT-Thread / JKXL for ka200_tools -u / Head640 Flash image")
    p.add_argument("input",
                   help="rtthread.bin (JKXL mode) or Serdes_Update JKXL (--head640)")
    p.add_argument("output", nargs="?", default=None,
                   help="output path (default depends on mode)")
    p.add_argument("--head640", action="store_true",
                   help="pack input JKXL into Head640 "
                        "(size+MD5+4K pad+body+512 pad), like ka200_tools repack")
    p.add_argument("--dest", type=lambda x: int(x, 0), default=DEFAULT_DEST)
    p.add_argument("--next-offset", type=lambda x: int(x, 0), default=DEFAULT_NEXT)
    p.add_argument("--version", type=lambda x: int(x, 0), default=VERSION)
    p.add_argument("--date", default=None,
                   help="YYYYMMDD in JKXL filename (default: today)")
    args = p.parse_args()

    if args.head640:
        out = args.output or default_head640_path(args.input)
        return pack_head640(args.input, out)

    out = args.output
    if not out:
        out = default_ka200_path(os.path.dirname(os.path.abspath(args.input)) or ".",
                                 args.version, args.date)

    return pack_ka200(args.input, out, args.dest, args.next_offset, args.version)


if __name__ == "__main__":
    sys.exit(main())
