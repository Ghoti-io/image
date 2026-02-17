#!/usr/bin/env python3
"""
Compare PROG_SYNC_ENC vs PROG_SYNC_DEC to find first encoder/decoder divergence.

Run the test with GIMG_JPEG_DEBUG_PROG_SYNC=1 and capture stderr, then:
  LD_LIBRARY_PATH=build/linux/release/apps GIMG_JPEG_DEBUG_PROG_SYNC=1 \
    build/linux/release/apps/testJpeg_encode \
    --gtest_filter=JpegEncode.ProgressiveWithRefinementScanDecodeMatchesBaseline 2> trace.txt
  python3 compare_prog_sync_debug.py trace.txt

Prints the first (scan, block) and line where ENC and DEC differ (byte, bit, or op/values).
"""

from __future__ import annotations

import argparse
import re
import sys

ENC_RE = re.compile(
    r"PROG_SYNC_ENC scan=(\d+) block=(\d+) byte=(\d+) bit=(\d+) op=(\w+)(.*)"
)
DEC_RE = re.compile(
    r"PROG_SYNC_DEC scan=(\d+) block=(\d+) byte=(\d+) bit=(\d+) op=(\w+)(.*)"
)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Find first PROG_SYNC ENC vs DEC divergence"
    )
    ap.add_argument(
        "trace_file",
        nargs="?",
        type=argparse.FileType("r"),
        default=sys.stdin,
        help="Trace file (stderr from test); default stdin",
    )
    args = ap.parse_args()

    enc_lines: list[tuple[int, int, int, int, str, str, str]] = []
    dec_lines: list[tuple[int, int, int, int, str, str, str]] = []

    with args.trace_file as f:
        for raw in f:
            line = raw.rstrip()
            m = ENC_RE.search(line)
            if m:
                enc_lines.append(
                    (
                        int(m.group(1)),
                        int(m.group(2)),
                        int(m.group(3)),
                        int(m.group(4)),
                        m.group(5),
                        m.group(6).strip(),
                        line,
                    )
                )
                continue
            m = DEC_RE.search(line)
            if m:
                dec_lines.append(
                    (
                        int(m.group(1)),
                        int(m.group(2)),
                        int(m.group(3)),
                        int(m.group(4)),
                        m.group(5),
                        m.group(6).strip(),
                        line,
                    )
                )

    if not enc_lines:
        print("No PROG_SYNC_ENC lines found.", file=sys.stderr)
        return 1
    if not dec_lines:
        print("No PROG_SYNC_DEC lines found.", file=sys.stderr)
        return 1

    # Group by (scan, block); decoder only has first MCU (e.g. blocks 0..5).
    def key(line: tuple) -> tuple[int, int]:
        return (line[0], line[1])  # scan, block

    enc_by_block: dict[tuple[int, int], list[tuple[int, int, str, str, str]]] = {}
    for s, b, y, t, op, rest, raw in enc_lines:
        k = (s, b)
        if k not in enc_by_block:
            enc_by_block[k] = []
        enc_by_block[k].append((y, t, op, rest, raw))

    dec_by_block: dict[tuple[int, int], list[tuple[int, int, str, str, str]]] = {}
    for s, b, y, t, op, rest, raw in dec_lines:
        k = (s, b)
        if k not in dec_by_block:
            dec_by_block[k] = []
        dec_by_block[k].append((y, t, op, rest, raw))

    # Compare (scan, block) in order of appearance in dec (first MCU).
    seen_blocks = set()
    for s, b, _y, _t, _op, _rest, raw in dec_lines:
        k = (s, b)
        if k in seen_blocks:
            continue
        seen_blocks.add(k)
        enc_ops = enc_by_block.get(k, [])
        dec_ops = dec_by_block.get(k, [])
        if not enc_ops:
            print("DEC has scan=%d block=%d but ENC has no ops for that block" % (s, b))
            print("  DEC lines:", dec_ops[:3])
            return 0
        if not dec_ops:
            print("ENC has scan=%d block=%d but DEC has no ops" % (s, b))
            return 0
        n = min(len(enc_ops), len(dec_ops))
        for i in range(n):
            ey, et, eop, erest, eline = enc_ops[i]
            dy, dt, dop, drest, dline = dec_ops[i]
            if (ey, et, eop) != (dy, dt, dop):
                print("First divergence at scan=%d block=%d op index %d" % (s, b, i))
                print("  ENC:", eline)
                print("  DEC:", dline)
                if (ey, et) != (dy, dt):
                    print("  -> position (byte, bit): enc (%d,%d) vs dec (%d,%d)" % (ey, et, dy, dt))
                if eop != dop:
                    print("  -> op: enc %s vs dec %s" % (eop, dop))
                return 0
        if len(enc_ops) != len(dec_ops):
            print("First length mismatch at scan=%d block=%d: ENC %d vs DEC %d ops" % (s, b, len(enc_ops), len(dec_ops)))
            if len(enc_ops) > len(dec_ops):
                print("  First extra ENC:", enc_ops[n][-1])
            else:
                print("  First extra DEC:", dec_ops[n][-1])
            return 0

    print("All compared (scan, block) ops match for first MCU.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
