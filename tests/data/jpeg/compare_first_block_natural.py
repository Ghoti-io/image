#!/usr/bin/env python3
"""
Compare our decoder's first block (zigzag) to libjpeg ref (natural order).

Our decoder stores coefficients in zigzag order; libjpeg jpeg_read_coefficients
returns blocks in natural (row-major) order. This script converts our block to
natural order and compares position-by-position to the ref.

Usage:
  python3 compare_first_block_natural.py <path-to-8x8-gray.jpg> [dump_jpeg_raster] [dump_jpeg_coef_ref]
  python3 compare_first_block_natural.py --bisect <path-to-8x8-gray.jpg> [dump_jpeg_raster] [dump_jpeg_coef_ref]

  --bisect: run our decoder with GIMG_JPEG_PROGRESSIVE_MAX_SCANS=1,2,...,N and report match count per K.

Requires: our decoder (with DUMP_JPEG_COEF_AFTER_SCAN=1), dump_jpeg_coef_ref (with DUMP_FIRST_BLOCK=1).
"""
import os
import re
import subprocess
import sys

# Our decoder's zigzag index -> natural index (from jpeg_entropy.c gimg_jpeg_zigzag)
# Natural index n corresponds to zigzag index inv_zigzag[n] (from gimg_jpeg_inv_zigzag)
INV_ZIGZAG = [
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
]


def get_ref_block(ref_tool: str, jpeg_path: str) -> list[int]:
    ref_env = os.environ.copy()
    ref_env["DUMP_FIRST_BLOCK"] = "1"
    r = subprocess.run(
        [ref_tool, jpeg_path],
        capture_output=True,
        text=True,
        timeout=10,
        env=ref_env,
    )
    if r.returncode != 0:
        raise SystemExit(f"Reference tool failed: {r.stderr or r.returncode}")
    m = re.search(r"FIRST_BLOCK\s+((?:-?\d+\s+)+)", r.stdout)
    if not m:
        raise SystemExit("Reference tool produced no FIRST_BLOCK")
    ref_natural = [int(x) for x in m.group(1).split()]
    if len(ref_natural) != 64:
        raise SystemExit(f"Ref block has {len(ref_natural)} values, expected 64")
    return ref_natural


def get_our_block_after_scans(decoder: str, jpeg_path: str, max_scans: int, env: dict) -> list[int]:
    e = env.copy()
    e["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    e["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scans)
    r = subprocess.run(
        [decoder, jpeg_path],
        capture_output=True,
        timeout=10,
        env=e,
    )
    stderr = (r.stderr or b"").decode("utf-8", errors="replace")
    blocks = re.findall(r"DUMP_JPEG_COEF_AFTER_SCAN scan\d+ block:\s*((?:-?\d+\s+)+)", stderr)
    if not blocks:
        raise SystemExit("Our decoder produced no DUMP_JPEG_COEF_AFTER_SCAN block line")
    our_zig = [int(x) for x in blocks[-1].split()]
    if len(our_zig) != 64:
        raise SystemExit(f"Our block has {len(our_zig)} values, expected 64")
    return [our_zig[INV_ZIGZAG[n]] for n in range(64)]


def main() -> int:
    argv = sys.argv[1:]
    bisect = False
    if argv and argv[0] == "--bisect":
        bisect = True
        argv = argv[1:]
    if len(argv) < 1:
        print("Usage: python3 compare_first_block_natural.py [--bisect] <8x8-gray.jpg> [dump_jpeg_raster] [dump_jpeg_coef_ref]", file=sys.stderr)
        return 2
    jpeg_path = os.path.abspath(argv[0])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    ref_tool = argv[2] if len(argv) >= 3 else os.path.join(script_dir, "dump_jpeg_coef_ref")
    if not os.path.isabs(ref_tool):
        ref_tool = os.path.join(script_dir, os.path.basename(ref_tool))
    ref_tool = os.path.abspath(ref_tool)

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(ref_tool):
        print(f"Reference tool not found: {ref_tool}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    ref_natural = get_ref_block(ref_tool, jpeg_path)

    if bisect:
        print("Bisect: match count vs ref (natural order) after K scans:")
        for k in range(1, 11):
            try:
                our_natural = get_our_block_after_scans(decoder, jpeg_path, k, env)
                match_count = sum(1 for a, b in zip(our_natural, ref_natural) if a == b)
                print(f"  MAX_SCANS={k}: {match_count}/64")
            except SystemExit as e:
                print(f"  MAX_SCANS={k}: {e}")
        return 0

    our_natural = get_our_block_after_scans(decoder, jpeg_path, 99, env)

    match_count = sum(1 for a, b in zip(our_natural, ref_natural) if a == b)
    diffs = [(n, our_natural[n], ref_natural[n]) for n in range(64) if our_natural[n] != ref_natural[n]]

    print(f"First block comparison (natural order): {match_count}/64 match")
    if diffs:
        print(f"  Differing positions (natural_idx, ours, ref):")
        for n, o, r in diffs[:24]:
            print(f"    [{n}] ours={o} ref={r}")
        if len(diffs) > 24:
            print(f"    ... and {len(diffs) - 24} more")
    else:
        print("  All 64 coefficients match.")
    return 0 if not diffs else 1


if __name__ == "__main__":
    sys.exit(main())
