#!/usr/bin/env python3
"""
Compare decoder first-MCU coefficients to encoder first-MCU dump (baseline 8x8 RGB).

Encoder: run SaveRgbThenLoadDecode with GIMG_JPEG_DUMP_FIRST_MCU_COEF=<dir>
  → writes block_0.bin .. block_5.bin (64 int16_t each, zigzag, LE).
Decoder: run dump_jpeg_raster on baseline_rgb.jpg with
  GIMG_JPEG_TRACE_BASELINE_FIRST_MCU=1
  → stderr has BLOCK_DEC linear=N ZIG: v0 v1 ... v63.

This script parses decoder stderr (from a file) and compares ZIG to encoder
block_*.bin. Reports first difference (block index, zig index, encoder val, decoder val).

Usage:
  python3 compare_baseline_first_mcu.py <decoder_stderr.txt> <encoder_coef_dir>
  Or pipe decoder stderr: ... 2>dec_stderr.txt; python3 compare_baseline_first_mcu.py dec_stderr.txt /tmp/enc_mcu
"""
from __future__ import annotations

import os
import re
import struct
import sys


def parse_decoder_stderr(stderr_path: str) -> dict[int, list[int]]:
    """Parse BLOCK_DEC linear=N ZIG: v0 v1 ... v63 -> {linear: [64 ints]}."""
    out: dict[int, list[int]] = {}
    with open(stderr_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"BLOCK_DEC linear=(\d+) comp=\d+ by=\d+ bx=\d+ ZIG:\s*(.+)", line)
            if m:
                linear = int(m.group(1))
                vals = [int(x) for x in m.group(2).split()]
                if len(vals) == 64:
                    out[linear] = vals
    return out


def read_encoder_block(path: str) -> list[int]:
    """Read block_N.bin: 64 int16_t LE."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != 128:
        raise ValueError(f"{path}: expected 128 bytes, got {len(data)}")
    return list(struct.unpack("<64h", data))


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: compare_baseline_first_mcu.py <decoder_stderr.txt> <encoder_coef_dir>", file=sys.stderr)
        return 2
    stderr_path = os.path.abspath(sys.argv[1])
    enc_dir = os.path.abspath(sys.argv[2])
    if not os.path.isfile(stderr_path):
        print(f"Not a file: {stderr_path}", file=sys.stderr)
        return 2
    if not os.path.isdir(enc_dir):
        print(f"Not a directory: {enc_dir}", file=sys.stderr)
        return 2

    dec_blocks = parse_decoder_stderr(stderr_path)
    if not dec_blocks:
        print("No BLOCK_DEC ZIG lines found in decoder stderr", file=sys.stderr)
        return 2

    all_match = True
    for linear in sorted(dec_blocks.keys()):
        enc_path = os.path.join(enc_dir, f"block_{linear}.bin")
        if not os.path.isfile(enc_path):
            print(f"Encoder block missing: {enc_path}", file=sys.stderr)
            all_match = False
            continue
        enc_zig = read_encoder_block(enc_path)
        dec_zig = dec_blocks[linear]
        for i in range(64):
            if enc_zig[i] != dec_zig[i]:
                print(f"Block {linear} zig[{i}]: encoder={enc_zig[i]} decoder={dec_zig[i]}")
                all_match = False
                break
        if all_match and linear in dec_blocks:
            print(f"Block {linear}: match (64 coeffs)")
    if all_match and dec_blocks:
        print("All blocks match.")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
