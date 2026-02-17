#!/usr/bin/env python3
"""
Compare progressive AC refinement (and AC initial) encoder vs decoder block positions.

Reads a trace file from GIMG_JPEG_TRACE_PROG_REFINE_SYNC=1 test run. Reports:
- AC initial scan: last block (byte, bit) enc vs dec; mismatch implies refinement
  scan will see different "already nonzero" counts (T.81 G.1.2.2).
- Refinement scan: first block where (byte, bit) differ or decoder fail.

Usage:
  GIMG_JPEG_TRACE_PROG_REFINE_SYNC=1 ./testJpeg_encode \
    --gtest_filter=JpegEncode.ProgressiveWithRefinementScanDecodeMatchesBaseline 2> trace.txt
  python3 compare_prog_refine_sync.py trace.txt
"""

from __future__ import annotations

import argparse
import re
import sys

AC_INITIAL_ENC_RE = re.compile(
    r"PROG_AC_INITIAL_ENC_BLOCK_AFTER block=(\d+) byte=(\d+) bit=(-?\d+)"
)
AC_INITIAL_DEC_RE = re.compile(
    r"PROG_AC_INITIAL_DEC_BLOCK_AFTER block=(\d+) byte=(\d+) bit=(\d+)"
)
ENC_RE = re.compile(r"PROG_REFINE_ENC_BLOCK_AFTER block=(\d+) byte=(\d+) bit=(-?\d+)")
DEC_RE = re.compile(r"PROG_REFINE_DEC_BLOCK_AFTER block=(\d+) byte=(\d+) bit=(\d+)")
DEC_FAIL_RE = re.compile(r"PROG_REFINE_DEC_FAIL block=(\d+) byte=(\d+) bit=(\d+)")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare progressive refinement ENC/DEC block positions"
    )
    ap.add_argument(
        "trace_file",
        nargs="?",
        type=argparse.FileType("r"),
        default=sys.stdin,
        help="Trace file (stderr from test run); default stdin",
    )
    args = ap.parse_args()

    ac_initial_enc: dict[int, tuple[int, int]] = {}
    ac_initial_dec: dict[int, tuple[int, int]] = {}
    enc: dict[int, tuple[int, int]] = {}
    dec: dict[int, tuple[int, int]] = {}
    dec_fail: list[tuple[int, int, int]] = []

    with args.trace_file as f:
        for line in f:
            m = AC_INITIAL_ENC_RE.search(line)
            if m:
                block, byte, bit = int(m.group(1)), int(m.group(2)), int(m.group(3))
                ac_initial_enc[block] = (byte, bit)
                continue
            m = AC_INITIAL_DEC_RE.search(line)
            if m:
                block, byte, bit = int(m.group(1)), int(m.group(2)), int(m.group(3))
                ac_initial_dec[block] = (byte, bit)
                continue
            m = ENC_RE.search(line)
            if m:
                block, byte, bit = int(m.group(1)), int(m.group(2)), int(m.group(3))
                enc[block] = (byte, bit)
                continue
            m = DEC_RE.search(line)
            if m:
                block, byte, bit = int(m.group(1)), int(m.group(2)), int(m.group(3))
                dec[block] = (byte, bit)
                continue
            m = DEC_FAIL_RE.search(line)
            if m:
                dec_fail.append(
                    (int(m.group(1)), int(m.group(2)), int(m.group(3)))
                )

    if ac_initial_enc and ac_initial_dec:
        last_ac = max(set(ac_initial_enc) & set(ac_initial_dec), default=None)
        if last_ac is not None:
            ae, ai = ac_initial_enc[last_ac]
            ad, aid = ac_initial_dec[last_ac]
            enc_bits = ae * 8 + max(0, ai)
            dec_bits = ad * 8 + aid
            print(f"AC initial (last block {last_ac}): enc byte={ae} bit={ai}  dec byte={ad} bit={aid}")
            if (ae, ai) != (ad, aid):
                print(f"  -> AC initial enc/dec position mismatch (enc {enc_bits} bits, dec {dec_bits} bits); refinement scan expects same 'already nonzero' count per block (T.81 G.1.2.2).")
            print()

    if not enc:
        print("No PROG_REFINE_ENC_BLOCK_AFTER lines found.", file=sys.stderr)
        return 1

    if not dec:
        print("No PROG_REFINE_DEC_BLOCK_AFTER lines found.", file=sys.stderr)
        if dec_fail:
            for (blk, by, bi) in dec_fail:
                print(f"Decoder failed at block {blk}: byte={by} bit={bi}", file=sys.stderr)
            if enc and 0 in enc:
                print(
                    f"Encoder after block 0: byte={enc[0][0]} bit={enc[0][1]}",
                    file=sys.stderr,
                )
        return 1

    blocks_enc = sorted(enc.keys())
    blocks_dec = sorted(dec.keys())
    print(
        f"Encoder: {len(blocks_enc)} blocks (first={blocks_enc[0]}, last={blocks_enc[-1]})"
    )
    print(
        f"Decoder: {len(blocks_dec)} blocks (first={blocks_dec[0]}, last={blocks_dec[-1]})"
    )

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
        common = [b for b in blocks_dec if b in enc]
        if not common:
            print("No common blocks.", file=sys.stderr)
            return 1
        last = common[-1]
        if enc[last] == dec[last]:
            print(
                f"\nAll {len(common)} decoded blocks match encoder position (last common block {last})."
            )
            if len(blocks_dec) < len(blocks_enc):
                print("Decoder failed before finishing; divergence at or after block", last + 1)
        else:
            print(f"\nDivergence at block {last}: enc={enc[last]} dec={dec[last]}")
        return 0

    for b in range(max(0, first_divergence - 2), first_divergence + 3):
        if b in enc and b in dec:
            print(
                f"  block {b}: enc byte={enc[b][0]} bit={enc[b][1]}  dec byte={dec[b][0]} bit={dec[b][1]}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
