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

Ref trace has no SCAN_ENTER/BLOCK_BEFORE; we identify an AC refinement scan by
REF_AC_REFINE_TABLE scan=N (ref uses 1-based scan index). For our 0-based scan
index S, ref uses scan N = S+1 (e.g. our scan 2 = refinement -> ref scan 3).
Blocks are delimited by REF_AC_REFINE_BLOCK_START / REF_AC_REFINE_AFTER_BLOCK0.

Usage:
  python3 compare_progressive_bits.py \
      --ours trace_ours.txt \
      --ref trace_ref.txt \
      --scan 2 --comp 0 --block 0

  For a 3-scan file (DC=0, AC initial=1, AC refinement=2) use --scan 2.
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
BLOCK_AFTER_RE = re.compile(
    r"BLOCK_AFTER scan(\d+)\s+mcu=\((\d+),(\d+)\)\s+comp=(\d+)\s+block=(\d+)\s+"
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
    """Extract full bit sequence (Huffman + refinement + correction) for (scan, comp, block).

    Block is the stream block index (0 = first block in scan, 1 = second, etc.),
    since for grayscale each MCU has one block and BLOCK_BEFORE block=0 repeats per MCU.
    """
    bits: List[BitRecord] = []
    cur_scan: Optional[int] = None
    stream_block_counter = 0
    cur_collecting = False

    for line_no, line in _iter_lines(path):
        m_scan = SCAN_ENTER_RE.match(line)
        if m_scan:
            cur_scan = int(m_scan.group(1))
            stream_block_counter = 0
            continue

        m_blk = BLOCK_BEFORE_RE.match(line)
        if m_blk:
            scan_b = int(m_blk.group(1))
            comp_b = int(m_blk.group(4))
            if scan_b == target_scan and comp_b == target_comp:
                cur_collecting = stream_block_counter == target_block
                stream_block_counter += 1
            else:
                cur_collecting = False
            cur_scan = scan_b
            continue

        m_after = BLOCK_AFTER_RE.match(line)
        if m_after:
            scan_a = int(m_after.group(1))
            comp_a = int(m_after.group(4))
            if scan_a == target_scan and comp_a == target_comp and cur_collecting:
                cur_collecting = False
                continue
            continue

        if cur_scan != target_scan or not cur_collecting:
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


def _ref_scan_block_regions(path: str, ref_scan: int) -> List[Tuple[int, int]]:
    """Find (start_line, end_line) for each AC-refine block in ref trace.

    ref_scan is libjpeg's 1-based scan number (e.g. 3 for our scan index 2).
    REF_AC_REFINE_TABLE scan=N identifies the scan; if absent (ref run without
    LIBJPEG_DEBUG_AC_TABLE), treat region after REF_COEF_AFTER_SCAN scan(N-1)
    until REF_COEF_AFTER_SCAN scan N as the refinement scan. Blocks are delimited
    by REF_AC_REFINE_BLOCK_START and REF_AC_REFINE_AFTER_BLOCK0.
    Returns list of (start, end) line numbers for comp-0 blocks.
    """
    in_scan = False
    block_starts: List[int] = []
    block_ends: List[int] = []
    ref_scan_str = f"scan={ref_scan}"
    # Fallback: ref uses 0-based scan in REF_COEF_AFTER_SCAN (scan0, scan1, scan2).
    # Our scan 2 (refinement) = ref 0-based scan 2; we enter after REF_COEF_AFTER_SCAN scan1.
    after_prev_scan = re.compile(r"REF_COEF_AFTER_SCAN\s+scan(\d+)")

    for line_no, line in _iter_lines(path):
        m_tbl = REF_AC_REFINE_TABLE_RE.search(line)
        if m_tbl and int(m_tbl.group(1)) == ref_scan:
            in_scan = True
            continue
        m_prev = after_prev_scan.search(line)
        if m_prev and int(m_prev.group(1)) == ref_scan - 2:
            in_scan = True
            continue
        if "REF_COEF_AFTER_SCAN" in line and f"scan{ref_scan - 1}" in line:
            break
        if not in_scan:
            continue
        if REF_AC_REFINE_BLOCK_START_RE.search(line):
            block_starts.append(line_no)
            continue
        if REF_AC_REFINE_AFTER_BLOCK0_RE.search(line):
            block_ends.append(line_no)
            continue
        if "REF_AC_REFINE_TABLE" in line and ref_scan_str not in line:
            break

    regions: List[Tuple[int, int]] = []
    for i, start in enumerate(block_starts):
        if i < len(block_ends):
            regions.append((start, block_ends[i]))
    return regions


def extract_ref_bits_ac_initial(path: str) -> List[BitRecord]:
    """Extract all REF_HUFF_BIT lines before REF_AC_REFINE_TABLE (AC initial scan).

    Ref emits REF_HUFF_BIT for AC initial when LIBJPEG_TRACE_HUFF_BITS=1 and
    trace start is set at first AC initial block. No block boundaries in ref;
    caller should slice to block 0 length using len(ours).
    """
    bits: List[BitRecord] = []
    for _line_no, line in _iter_lines(path):
        if "REF_AC_REFINE_TABLE" in line:
            break
        s = line.strip()
        m = REF_HUFF_BIT_RE.match(s)
        if m:
            bits.append(
                BitRecord(
                    bit=int(m.group(2)),
                    byte_off=int(m.group(3)),
                    bit_off=int(m.group(4)),
                    line_no=_line_no,
                    line=line,
                )
            )
    return bits


def extract_ref_bits(
    path: str, target_scan: int, target_comp: int, target_block: int
) -> List[BitRecord]:
    """Extract bit sequence for (scan, comp, block) from ref using REF_* markers.

    For scan 1 (AC initial): return first len(ours) bits from ref AC initial
    stream (ref has no block boundaries; we slice by ours block 0 length in main).
    For refinement (scan 2): ref_scan = target_scan + 1, use REF_AC_REFINE_*
    to delimit blocks. Only comp 0 is supported for ref.
    """
    if target_comp != 0:
        return []

    if target_scan == 1:
        return extract_ref_bits_ac_initial(path)

    ref_scan = target_scan + 1
    regions = _ref_scan_block_regions(path, ref_scan)
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
    p.add_argument(
        "--scan",
        type=int,
        default=2,
        help="Our 0-based scan index (e.g. 2 for AC refinement in 3-scan file)",
    )
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

    # AC initial (scan 1): ref has no block boundaries; ref_bits are all bits in order. Slice to this block.
    if args.scan == 1 and ref_bits and ours_bits:
        offset = 0
        for b in range(0, args.block):
            offset += len(extract_ours_bits(args.ours, 1, args.comp, b))
        ref_bits = ref_bits[offset : offset + len(ours_bits)]

    print(
        f"Comparing bits for scan={args.scan}, comp={args.comp}, block={args.block}"
    )
    compare_bits(ours_bits, ref_bits)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

