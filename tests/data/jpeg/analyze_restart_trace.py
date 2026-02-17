#!/usr/bin/env python3
"""
Analyze decoder trace from baseline-with-restart test (HUFF_BIT, DEC_BLOCK_POS, DEC_BLOCK_AFTER).

Use when our decoder fails on a DRI/RST file: run the test with
  GIMG_JPEG_TRACE_HUFF_BITS=1 GIMG_JPEG_TRACE_DEC_BLOCK_POS=1
and redirect stderr to a file, then run this script on that file.

Reports:
- DEC_BLOCK_POS / DEC_BLOCK_AFTER: block index, byte_off, bit_off, cumulative bits.
- HUFF_BIT count per block (if trace contains HUFF_BIT lines).
- Last position before any failure (useful to compare with encoder: 32×32 grayscale
  restart_interval=4 uses 4 MCUs = 4 blocks before first RST; encoder typically
  uses ~88 bits for blocks 0–3, so decoder at 82 bits indicates 6-bit shortage).

Usage:
  GIMG_JPEG_TRACE_HUFF_BITS=1 GIMG_JPEG_TRACE_DEC_BLOCK_POS=1 \
    ./build/linux/release/apps/testJpeg_encode \
    --gtest_filter=JpegEncode.EncodeRestartIntervalThenLoadDecodeAndLibjpegOracle 2> trace.txt
  python3 tests/data/jpeg/analyze_restart_trace.py trace.txt
"""

from __future__ import annotations

import argparse
import re
import sys

DEC_BLOCK_POS_RE = re.compile(
    r"DEC_BLOCK_POS block=(\d+) pos_bits=(\d+) byte_off=(\d+) bit_off=(\d+)"
)
DEC_BLOCK_AFTER_RE = re.compile(
    r"DEC_BLOCK_AFTER block=(\d+) byte=(\d+) bit=(\d+)"
)
HUFF_BIT_RE = re.compile(
    r"HUFF_BIT\s+len=(\d+)\s+bit=(\d+)\s+code=0x[0-9a-fA-F]+\s+byte_off=(\d+)\s+bit_off=(\d+)"
)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Analyze baseline-with-restart decoder trace (DEC_BLOCK_*, HUFF_BIT)"
    )
    ap.add_argument(
        "trace_file",
        nargs="?",
        type=argparse.FileType("r"),
        default=sys.stdin,
        help="Trace file (stderr from test run); default stdin",
    )
    args = ap.parse_args()

    block_pos: dict[int, tuple[int, int, int]] = {}  # block -> (byte_off, bit_off, pos_bits)
    block_after: dict[int, tuple[int, int]] = {}    # block -> (byte, bit)
    huff_bits_per_block: dict[int, int] = {}         # block -> count of HUFF_BIT lines
    current_block = -1
    last_huff_bit: tuple[int, int, int] | None = None  # (byte_off, bit_off, cumulative_bits)
    huff_bit_count = 0

    with args.trace_file as f:
        for line in f:
            m = DEC_BLOCK_POS_RE.search(line)
            if m:
                block, pos_bits, byte_off, bit_off = (
                    int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
                )
                block_pos[block] = (byte_off, bit_off, pos_bits)
                current_block = block
                huff_bits_per_block[block] = huff_bits_per_block.get(block, 0)
                continue
            m = DEC_BLOCK_AFTER_RE.search(line)
            if m:
                block, byte_off, bit_off = int(m.group(1)), int(m.group(2)), int(m.group(3))
                block_after[block] = (byte_off, bit_off)
                continue
            m = HUFF_BIT_RE.search(line)
            if m:
                byte_off, bit_off = int(m.group(3)), int(m.group(4))
                cum = byte_off * 8 + bit_off
                last_huff_bit = (byte_off, bit_off, cum)
                if current_block >= 0:
                    huff_bits_per_block[current_block] = huff_bits_per_block.get(current_block, 0) + 1
                huff_bit_count += 1
                continue

    # Report DEC_BLOCK_POS / DEC_BLOCK_AFTER
    if block_pos or block_after:
        blocks = sorted(set(block_pos.keys()) | set(block_after.keys()))
        print("Block positions (decoder)")
        print("  block  byte_off  bit_off  cumulative_bits  (source)")
        for b in blocks:
            pos = block_pos.get(b)
            after = block_after.get(b)
            if pos is not None:
                byte_off, bit_off, pos_bits = pos
                cum = byte_off * 8 + bit_off
                src = "DEC_BLOCK_POS"
                if after is not None:
                    src += " / DEC_BLOCK_AFTER"
                print(f"  {b:5}  {byte_off:8}  {bit_off:6}  {cum:15}  ({src})")
            elif after is not None:
                byte_off, bit_off = after
                cum = byte_off * 8 + bit_off
                print(f"  {b:5}  {byte_off:8}  {bit_off:6}  {cum:15}  (DEC_BLOCK_AFTER)")
        if blocks:
            last_b = max(blocks)
            if last_b in block_after:
                by, bi = block_after[last_b]
                print(f"\nLast completed block: {last_b}  ->  byte={by} bit={bi}  ({by*8+bi} bits)")
            else:
                print(f"\nLast block position logged: {last_b} (block not completed; decode may have failed)")
    else:
        print("No DEC_BLOCK_POS or DEC_BLOCK_AFTER lines found.")

    if huff_bits_per_block:
        print("\nHUFF_BIT count per block (while decoding that block):")
        for b in sorted(huff_bits_per_block.keys()):
            print(f"  block {b}: {huff_bits_per_block[b]} HUFF_BIT lines")

    if last_huff_bit is not None:
        by, bi, cum = last_huff_bit
        print(f"\nLast HUFF_BIT position: byte_off={by} bit_off={bi}  (cumulative bits from scan start: {cum})")
        print(f"Total HUFF_BIT lines: {huff_bit_count}")

    # 32×32 grayscale, restart_interval=4: 4 blocks then RST
    print("\nNote: 32×32 grayscale with restart_interval=4 has 4 blocks before first RST.")
    print("Encoder typically uses ~88 bits for blocks 0–3; decoder at 82 bits => 6-bit shortage.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
