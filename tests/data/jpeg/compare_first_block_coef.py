#!/usr/bin/env python3
"""
Compare first-block coefficient dumps: ours (GIMG_JPEG_DUMP_FIRST_BLOCK_COEF)
vs libjpeg (LIBJPEG_DUMP_FIRST_BLOCK_COEF when using encode_libjpeg_baseline_scan_debug).

Both dumps are 64 x int16_t in zigzag order. Exit 0 if match, 1 if difference.
"""
import argparse
import struct
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare first-block coef dumps (64 x int16, zigzag)")
    ap.add_argument("ours", help="Path to our encoder first-block dump")
    ap.add_argument("ref", help="Path to libjpeg encoder first-block dump")
    args = ap.parse_args()
    with open(args.ours, "rb") as f:
        ours_b = f.read()
    with open(args.ref, "rb") as f:
        ref_b = f.read()
    if len(ours_b) != 128 or len(ref_b) != 128:
        print(f"Expected 128 bytes each; got ours={len(ours_b)} ref={len(ref_b)}")
        return 1
    ours = list(struct.unpack("<64h", ours_b))
    ref = list(struct.unpack("<64h", ref_b))
    for i in range(64):
        if ours[i] != ref[i]:
            print(f"First difference at zigzag index {i}: ours={ours[i]} ref={ref[i]}")
            print(f"  DC (index 0): ours={ours[0]} ref={ref[0]}")
            return 1
    print("Match: 64 coefficients identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
