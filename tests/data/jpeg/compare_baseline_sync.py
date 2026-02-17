#!/usr/bin/env python3
"""
Compare baseline encoder vs decoder block positions (bit-level sync).

Reads a trace file containing ENC_BLOCK_AFTER and DEC_BLOCK_AFTER lines
(from stderr when running the restart test with GIMG_JPEG_TRACE_BASELINE_SYNC=1).
Reports the first block where (byte, bit) after that block differ between
encoder and decoder, to pinpoint where the bitstream diverges.

Usage:
  GIMG_JPEG_TRACE_BASELINE_SYNC=1 ./test_jpeg_encode \
    --gtest_filter=JpegEncode.Large640x480BaselineWithRestart 2> trace_sync.txt
  python3 compare_baseline_sync.py trace_sync.txt

Or:
  python3 compare_baseline_sync.py < trace_sync.txt
"""

from __future__ import annotations

import argparse
import re
import sys

ENC_RE = re.compile(r"ENC_BLOCK_AFTER block=(\d+) byte=(\d+) bit=(-?\d+)")
DEC_RE = re.compile(r"DEC_BLOCK_AFTER block=(\d+) byte=(\d+) bit=(\d+)")


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare baseline ENC/DEC block positions")
    ap.add_argument(
        "trace_file",
        nargs="?",
        type=argparse.FileType("r"),
        default=sys.stdin,
        help="Trace file (stderr from test run); default stdin",
    )
    args = ap.parse_args()

    enc: dict[int, tuple[int, int]] = {}
    dec: dict[int, tuple[int, int]] = {}

    with args.trace_file as f:
        for line in f:
            m = ENC_RE.search(line)
            if m:
                block, byte, bit = int(m.group(1)), int(m.group(2)), int(m.group(3))
                enc[block] = (byte, bit)
                continue
            m = DEC_RE.search(line)
            if m:
                block, byte, bit = int(m.group(1)), int(m.group(2)), int(m.group(3))
                dec[block] = (byte, bit)

    if not enc:
        print("No ENC_BLOCK_AFTER lines found.", file=sys.stderr)
        return 1
    if not dec:
        print("No DEC_BLOCK_AFTER lines found.", file=sys.stderr)
        return 1

    blocks_enc = sorted(enc.keys())
    blocks_dec = sorted(dec.keys())
    print(f"Encoder: {len(blocks_enc)} blocks (first={blocks_enc[0]}, last={blocks_enc[-1]})")
    print(f"Decoder: {len(blocks_dec)} blocks (first={blocks_dec[0]}, last={blocks_dec[-1]})")

    first_divergence: int | None = None
    for b in blocks_dec:
        if b not in enc:
            continue
        be, bi = enc[b]
        bd, bid = dec[b]
        if (be, bi) != (bd, bid):
            first_divergence = b
            print(f"\nFirst divergence after block {b}:")
            print(f"  Encoder: byte={be} bit={bi}")
            print(f"  Decoder: byte={bd} bit={bid}")
            break

    if first_divergence is None:
        # Decoder may have failed before finishing; compare last common block
        common = [b for b in blocks_dec if b in enc]
        if not common:
            print("No common blocks.", file=sys.stderr)
            return 1
        last = common[-1]
        if enc[last] == dec[last]:
            print(f"\nAll {len(common)} decoded blocks match encoder position (last common block {last}).")
            print("Decoder failed on the next block; divergence is at or after block", last + 1)
        else:
            print(f"\nDivergence at block {last}: enc={enc[last]} dec={dec[last]}")
        return 0

    # Optional: show a short window around first divergence
    for b in range(max(0, first_divergence - 2), first_divergence + 3):
        if b in enc and b in dec:
            print(f"  block {b}: enc byte={enc[b][0]} bit={enc[b][1]}  dec byte={dec[b][0]} bit={dec[b][1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
