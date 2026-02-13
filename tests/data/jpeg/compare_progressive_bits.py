#!/usr/bin/env python3
"""
Bit-level comparison for progressive JPEG traces.

Goal:
- For a given scan / component / block, extract the full bit sequence (Huffman +
  refinement + correction) from our trace and the libjpeg ref trace, then
  report where they diverge.

Inputs:
- Our trace: produced by running the decoder with GIMG_JPEG_TRACE_ALL=1
  (or at least GIMG_JPEG_TRACE_HUFF_BITS=1 plus the progressive trace envs).
- Ref trace: produced by dump_jpeg_coef_ref_debug with LIBJPEG_TRACE_HUFF_BITS=1
  (emits REF_HUFF_BIT, REF_HUFF_MATCH; when slow path is used for first block
  also REF_AC_REFINE_REFINEMENT_BIT / REF_AC_REFINE_CORRECTION_BIT).

Ref trace has no SCAN_ENTER/BLOCK_BEFORE; we identify scan 5 by
REF_AC_REFINE_TABLE scan=6 (ref uses 1-based scan index) and then
REF_AC_REFINE_BLOCK_START / REF_AC_REFINE_AFTER_BLOCK0 to delimit blocks.

Usage:
  python3 compare_progressive_bits.py \
      --ours trace_ours.txt \
      --ref trace_ref.txt \
      --scan 5 --comp 0 --block 0

You can run with --block 1 to compare the next block.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from typing import Iterable, List, Optional, Tuple


SCAN_ENTER_RE = re.compile(
    r"SCAN_ENTER scan(\d+)\s+Ss=(\d+)\s+Se=(\d+)\s+Ah=(\d+)\s+Al=(\d+)\s+is_dc=(\d+)"
)

BLOCK_BEFORE_RE = re.compile(
    r"BLOCK_BEFORE scan(\d+)\s+mcu=\((\d+),(\d+)\)\s+comp=(\d+)\s+block=(\d+)\s+"
)

HUFF_BIT_RE = re.compile(
    r"HUFF_BIT\s+len=(\d+)\s+bit=(\d+)\s+code=0x[0-9a-fA-F]+\s+byte_off=(\d+)\s+bit_off=(\d+)"
)

AC_REFINE_REFINEMENT_BIT_RE = re.compile(
    r"AC_REFINE_REFINEMENT_BIT\s+block=\d+\s+bit=(\d+)\s+byte_off=(\d+)\s+bit_off=(\d+)"
)

AC_REFINE_CORRECTION_BIT_RE = re.compile(
    r"AC_REFINE_CORRECTION_BIT\s+block=\d+\s+k=\d+\s+bit=(\d+).*byte_off=(\d+)\s+bit_off=(\d+)"
)

REF_HUFF_BIT_RE = re.compile(
    r"REF_HUFF_BIT\s+len=(\d+)\s+bit=(\d+)\s+code=0x[0-9a-fA-F]+\s+byte_off=(\d+)\s+bit_off=(\d+)"
)

REF_AC_REFINE_REFINEMENT_BIT_RE = re.compile(
    r"REF_AC_REFINE_REFINEMENT_BIT\s+bit=(\d+)\s+byte_off=(\d+)\s+bit_off=(\d+)"
)

REF_AC_REFINE_CORRECTION_BIT_RE = re.compile(
    r"REF_AC_REFINE_CORRECTION_BIT\s+k=\d+\s+bit=(\d+)\s+byte_off=(\d+)\s+bit_off=(\d+)"
)

REF_AC_REFINE_TABLE_RE = re.compile(r"REF_AC_REFINE_TABLE\s+scan=(\d+)")
REF_AC_REFINE_BLOCK_START_RE = re.compile(r"REF_AC_REFINE_BLOCK_START\s+block=(\d+)")
REF_AC_REFINE_AFTER_BLOCK0_RE = re.compile(r"REF_AC_REFINE_AFTER_BLOCK0")


@dataclass
class BitRecord:
    bit: int
    byte_off: int
    bit_off: int
    line_no: int
    line: str


def _iter_lines(path: str) -> Iterable[Tuple[int, str]]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, start=1):
            yield i, line.rstrip("\n")


def extract_ours_bits(
    path: str, target_scan: int, target_comp: int, target_block: int
) -> List[BitRecord]:
    """Extract full bit sequence (Huffman + refinement + correction) for (scan, comp, block)."""
    bits: List[BitRecord] = []
    cur_scan: Optional[int] = None
    cur_comp: Optional[int] = None
    cur_block: Optional[int] = None

    for line_no, line in _iter_lines(path):
        m_scan = SCAN_ENTER_RE.match(line)
        if m_scan:
            cur_scan = int(m_scan.group(1))
            continue

        m_blk = BLOCK_BEFORE_RE.match(line)
        if m_blk:
            cur_scan = int(m_blk.group(1))
            mcu_x = int(m_blk.group(2))
            mcu_y = int(m_blk.group(3))
            comp = int(m_blk.group(4))
            block = int(m_blk.group(5))
            if mcu_x == 0 and mcu_y == 0:
                cur_comp = comp
                cur_block = block
            else:
                cur_comp = None
                cur_block = None
            continue

        if (
            cur_scan != target_scan
            or cur_comp != target_comp
            or cur_block != target_block
        ):
            continue

        s = line.strip()

        m_bit = HUFF_BIT_RE.match(s)
        if m_bit:
            bit = int(m_bit.group(2))
            bits.append(
                BitRecord(
                    bit=bit,
                    byte_off=int(m_bit.group(3)),
                    bit_off=int(m_bit.group(4)),
                    line_no=line_no,
                    line=line,
                )
            )
            continue

        m_ref = AC_REFINE_REFINEMENT_BIT_RE.match(s)
        if m_ref:
            bit = int(m_ref.group(1))
            bits.append(
                BitRecord(
                    bit=bit,
                    byte_off=int(m_ref.group(2)),
                    bit_off=int(m_ref.group(3)),
                    line_no=line_no,
                    line=line,
                )
            )
            continue

        m_corr = AC_REFINE_CORRECTION_BIT_RE.match(s)
        if m_corr:
            bit = int(m_corr.group(1))
            bits.append(
                BitRecord(
                    bit=bit,
                    byte_off=int(m_corr.group(2)),
                    bit_off=int(m_corr.group(3)),
                    line_no=line_no,
                    line=line,
                )
            )

    return bits


def _ref_scan5_block_regions(path: str) -> List[Tuple[int, int]]:
    """Find (start_line, end_line) for each scan-5 AC-refine block in ref trace.

    Scan 5 is identified by REF_AC_REFINE_TABLE scan=6 (ref uses 1-based).
    Blocks are delimited by REF_AC_REFINE_BLOCK_START and REF_AC_REFINE_AFTER_BLOCK0.
    Returns list of (start, end) line numbers for comp-0 blocks (first N blocks
    after the table).
    """
    in_scan5 = False
    block_starts: List[int] = []
    block_ends: List[int] = []

    for line_no, line in _iter_lines(path):
        m_tbl = REF_AC_REFINE_TABLE_RE.search(line)
        if m_tbl and int(m_tbl.group(1)) == 6:
            in_scan5 = True
            continue
        if not in_scan5:
            continue
        if REF_AC_REFINE_BLOCK_START_RE.search(line):
            block_starts.append(line_no)
            continue
        if REF_AC_REFINE_AFTER_BLOCK0_RE.search(line):
            block_ends.append(line_no)
            continue
        # If we see another scan's table or coef dump, stop
        if "REF_COEF_AFTER_SCAN" in line or "REF_AC_REFINE_TABLE" in line:
            if "REF_AC_REFINE_TABLE" in line and "scan=6" not in line:
                break

    regions: List[Tuple[int, int]] = []
    for i, start in enumerate(block_starts):
        if i < len(block_ends):
            regions.append((start, block_ends[i]))
        if len(regions) >= 4:
            break
    return regions


def extract_ref_bits(
    path: str, target_scan: int, target_comp: int, target_block: int
) -> List[BitRecord]:
    """Extract bit sequence for (scan, comp, block) from ref using REF_* markers.

    For scan 5 we use REF_AC_REFINE_TABLE scan=6 and REF_AC_REFINE_BLOCK_START/
    REF_AC_REFINE_AFTER_BLOCK0 to get block 0 and 1. We collect REF_HUFF_BIT,
    REF_AC_REFINE_REFINEMENT_BIT, REF_AC_REFINE_CORRECTION_BIT in order.
    If ref used the fast path there may be no REF_HUFF_BIT; we still collect
    refinement/correction when present.
    """
    if target_scan != 5 or target_comp != 0:
        return []

    regions = _ref_scan5_block_regions(path)
    if target_block >= len(regions):
        return []

    start_line, end_line = regions[target_block]
    bits: List[BitRecord] = []

    for line_no, line in _iter_lines(path):
        if line_no < start_line:
            continue
        if line_no > end_line:
            break

        s = line.strip()

        for pattern, grp_bit, grp_bo, grp_bio in [
            (REF_HUFF_BIT_RE, 2, 3, 4),
            (REF_AC_REFINE_REFINEMENT_BIT_RE, 1, 2, 3),
            (REF_AC_REFINE_CORRECTION_BIT_RE, 1, 2, 3),
        ]:
            m = pattern.match(s)
            if m:
                bit = int(m.group(grp_bit))
                bits.append(
                    BitRecord(
                        bit=bit,
                        byte_off=int(m.group(grp_bo)),
                        bit_off=int(m.group(grp_bio)),
                        line_no=line_no,
                        line=line,
                    )
                )
                break

    return bits


def compare_bits(ours: List[BitRecord], ref: List[BitRecord]) -> None:
    print(f"ours bits: {len(ours)}, ref bits: {len(ref)}")
    if not ours:
        print("WARNING: no HUFF_BIT lines found in our trace for this block.")
    if not ref:
        print("WARNING: no ref bit lines (REF_HUFF_BIT / REF_AC_REFINE_* ) in ref trace for this block.")

    n = min(len(ours), len(ref))
    mismatch_idx: Optional[int] = None
    for i in range(n):
        if ours[i].bit != ref[i].bit:
            mismatch_idx = i
            break

    if mismatch_idx is None:
        if len(ours) == len(ref):
            print("Bits match exactly (same length and content).")
            return
        else:
            print(
                "Bit prefixes match but lengths differ: "
                f"ours={len(ours)}, ref={len(ref)}."
            )
            return

    print(f"First differing bit at index {mismatch_idx}:")
    print(
        f"  ours: bit={ours[mismatch_idx].bit} "
        f"(byte_off={ours[mismatch_idx].byte_off}, "
        f"bit_off={ours[mismatch_idx].bit_off}, "
        f"line={ours[mismatch_idx].line_no})"
    )
    print(
        f"  ref:  bit={ref[mismatch_idx].bit} "
        f"(byte_off={ref[mismatch_idx].byte_off}, "
        f"bit_off={ref[mismatch_idx].bit_off}, "
        f"line={ref[mismatch_idx].line_no})"
    )

    # Show a small window around the mismatch for context.
    window = 8
    start = max(0, mismatch_idx - window)
    end = min(max(len(ours), len(ref)), mismatch_idx + window + 1)

    print("\nContext around mismatch (index, ours_bit, ref_bit):")
    for i in range(start, end):
        ob = ours[i].bit if i < len(ours) else "-"
        rb = ref[i].bit if i < len(ref) else "-"
        prefix = ">>" if i == mismatch_idx else "  "
        print(f"{prefix} i={i:4d}: ours={ob}  ref={rb}")


def main() -> int:
    p = argparse.ArgumentParser(description="Bit-level progressive JPEG trace diff.")
    p.add_argument("--ours", required=True, help="Path to our trace_ours.txt")
    p.add_argument("--ref", required=True, help="Path to trace_ref.txt")
    p.add_argument("--scan", type=int, default=5, help="Scan index (default: 5)")
    p.add_argument("--comp", type=int, default=0, help="Component index (default: 0)")
    p.add_argument(
        "--block",
        type=int,
        default=0,
        help="Block index in first MCU (default: 0)",
    )
    args = p.parse_args()

    ours_bits = extract_ours_bits(args.ours, args.scan, args.comp, args.block)
    ref_bits = extract_ref_bits(args.ref, args.scan, args.comp, args.block)

    print(
        f"Comparing bits for scan={args.scan}, comp={args.comp}, block={args.block}"
    )
    compare_bits(ours_bits, ref_bits)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

