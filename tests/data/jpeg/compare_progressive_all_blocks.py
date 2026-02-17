#!/usr/bin/env python3
"""
Compare ALL coefficient blocks (ours vs ref) after each scan to find the first
divergence. Uses detailed debug output from both decoders.

Requires:
- Our decoder run with GIMG_JPEG_TRACE_ALL=1 (emits BLOCK_AFTER for every block).
- Ref (dump_jpeg_coef_ref_debug) run with DUMP_JPEG_COEF_AFTER_SCAN=1 and
  DUMP_JPEG_COEF_ALL_BLOCKS_AFTER_SCAN=1 (emits REF_SCANn_COMPc_BLOCKb for every block).

Usage:
  python3 compare_progressive_all_blocks.py --ours trace_ours.txt --ref trace_ref.txt
  python3 compare_progressive_all_blocks.py --ours trace_ours.txt --ref trace_ref.txt --verbose

Exit 0: all blocks match. Exit 1: first mismatch found (scan, block index, coeff index).
"""
from __future__ import annotations

import argparse
import re
import sys

BLOCK_AFTER_RE = re.compile(
    r"BLOCK_AFTER scan(\d+) mcu=\((\d+),(\d+)\) comp=(\d+) block=(\d+) "
    r"byte_off=\d+ bit_off=\d+ coeffs:\s*(.*)"
)
REF_BLOCK_RE = re.compile(r"REF_SCAN(\d+)_COMP(\d+)_BLOCK(\d+)\s+(.*)")


def collect_ours_blocks(stderr_text: str) -> list[tuple[int, int, int, list[int]]]:
    """Collect (scan, block_linear, comp, coeffs) in decode order.
    block_linear = mcu_y * mcu_width + mcu_x; we infer dimensions from max mcu_x/mcu_y."""
    matches = []
    for line in stderr_text.splitlines():
        m = BLOCK_AFTER_RE.match(line.strip())
        if not m:
            continue
        scan, mcu_x, mcu_y, comp, block_in_mcu, coeff_str = m.groups()
        scan = int(scan)
        mcu_x = int(mcu_x)
        mcu_y = int(mcu_y)
        comp = int(comp)
        coeffs = [int(x) for x in coeff_str.split()]
        if len(coeffs) != 64:
            continue
        matches.append((scan, mcu_x, mcu_y, comp, coeffs))
    if not matches:
        return []
    max_mcu_x = max(mcu_x for (_, mcu_x, _, _, _) in matches)
    max_mcu_y = max(mcu_y for (_, _, mcu_y, _, _) in matches)
    mcu_width = max_mcu_x + 1
    return [
        (scan, mcu_y * mcu_width + mcu_x, comp, coeffs)
        for (scan, mcu_x, mcu_y, comp, coeffs) in matches
    ]


def collect_ref_blocks(stderr_text: str) -> list[tuple[int, int, int, list[int]]]:
    """Collect (scan, block_idx, comp, coeffs) from REF_SCANn_COMPc_BLOCKb lines."""
    blocks = []
    for line in stderr_text.splitlines():
        m = REF_BLOCK_RE.match(line.strip())
        if not m:
            continue
        scan, comp, block_idx, coeff_str = m.groups()
        scan = int(scan)
        comp = int(comp)
        block_idx = int(block_idx)
        coeffs = [int(x) for x in coeff_str.split()]
        if len(coeffs) != 64:
            continue
        blocks.append((scan, block_idx, comp, coeffs))
    return blocks


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare all blocks (ours vs ref) to find first divergence.")
    ap.add_argument("--ours", required=True, help="Path to our decoder stderr (GIMG_JPEG_TRACE_ALL=1)")
    ap.add_argument("--ref", required=True, help="Path to ref stderr (DUMP_JPEG_COEF_ALL_BLOCKS_AFTER_SCAN=1)")
    ap.add_argument("--verbose", "-v", action="store_true", help="Print first few coeffs at mismatch")
    args = ap.parse_args()

    with open(args.ours, "r", encoding="utf-8", errors="replace") as f:
        ours_text = f.read()
    with open(args.ref, "r", encoding="utf-8", errors="replace") as f:
        ref_text = f.read()

    ours_blocks = collect_ours_blocks(ours_text)
    ref_blocks = collect_ref_blocks(ref_text)

    if not ours_blocks:
        print("No BLOCK_AFTER lines in ours trace.", file=sys.stderr)
        return 2
    if not ref_blocks:
        print("No REF_SCAN*_COMP*_BLOCK* lines in ref trace. Run ref with DUMP_JPEG_COEF_ALL_BLOCKS_AFTER_SCAN=1", file=sys.stderr)
        return 2

    # Ref may have only ALL_BLOCKS lines (no first-MCU duplicate). Ours has one BLOCK_AFTER per block decode.
    # Order: ours is decode order (scan 0 blk0, blk1, ... scan 1 blk0, ...). Ref is same (scan, block_idx).
    # Normalize to (scan, block_linear, comp) and sort so we can compare.
    def key_ours(t: tuple) -> tuple:
        scan, block_linear, comp, _ = t
        return (scan, block_linear, comp)

    def key_ref(t: tuple) -> tuple:
        scan, block_idx, comp, _ = t
        return (scan, block_idx, comp)

    ours_sorted = sorted(ours_blocks, key=key_ours)
    ref_sorted = sorted(ref_blocks, key=key_ref)

    # Dedupe: ours can have multiple BLOCK_AFTER per (scan, block) if we trace multiple times; take last.
    ours_dedup = {}
    for t in ours_sorted:
        k = key_ours(t)
        ours_dedup[k] = t[3]  # coeffs only
    ref_dedup = {}
    for t in ref_sorted:
        k = key_ref(t)
        ref_dedup[k] = t[3]

    keys_ours = sorted(ours_dedup.keys())
    keys_ref = sorted(ref_dedup.keys())
    if keys_ours != keys_ref:
        print(f"Block key sets differ: ours {len(keys_ours)} blocks, ref {len(keys_ref)} blocks", file=sys.stderr)
        only_ours = set(keys_ours) - set(keys_ref)
        only_ref = set(keys_ref) - set(keys_ours)
        if only_ours:
            print(f"  Only in ours: {sorted(only_ours)[:10]}...", file=sys.stderr)
        if only_ref:
            print(f"  Only in ref: {sorted(only_ref)[:10]}...", file=sys.stderr)
        # Continue and compare common keys
        common_keys = sorted(set(keys_ours) & set(keys_ref))
    else:
        common_keys = keys_ours

    for k in common_keys:
        ours_coeffs = ours_dedup[k]
        ref_coeffs = ref_dedup[k]
        if ours_coeffs != ref_coeffs:
            scan, block_idx, comp = k
            first_coeff = None
            for i in range(64):
                if ours_coeffs[i] != ref_coeffs[i]:
                    first_coeff = i
                    break
            print(f"First divergence: scan={scan} block={block_idx} comp={comp} coeff_index={first_coeff}", file=sys.stderr)
            if first_coeff is not None and args.verbose:
                print(f"  ours[{first_coeff}]={ours_coeffs[first_coeff]} ref[{first_coeff}]={ref_coeffs[first_coeff]}", file=sys.stderr)
                # Show a short window
                lo = max(0, first_coeff - 2)
                hi = min(64, first_coeff + 3)
                print(f"  ours coeffs[{lo}:{hi}]: {ours_coeffs[lo:hi]}", file=sys.stderr)
                print(f"  ref  coeffs[{lo}:{hi}]: {ref_coeffs[lo:hi]}", file=sys.stderr)
            return 1

    print(f"All {len(common_keys)} blocks match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
