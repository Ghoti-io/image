#!/usr/bin/env python3
"""
Compare two binary files of 64 x int16_t (128 bytes, little-endian).
Use for first-block presamples (Step 1) or pre-quant DCT (Step 3) when both
dumps are in the same order (e.g. row-major / natural).

  python3 compare_64_int16_bin.py ours.bin ref.bin
  python3 compare_64_int16_bin.py ours.bin ref.bin --order natural

Exit 0 if match, 1 if difference or wrong size.
"""
import argparse
import struct
import sys


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare two 64×int16 binary dumps (128 bytes each)"
    )
    ap.add_argument("ours", help="Path to first dump")
    ap.add_argument("ref", help="Path to second dump")
    ap.add_argument(
        "--order",
        choices=("natural", "zigzag"),
        default="natural",
        help="Index order for reporting (default: natural)",
    )
    args = ap.parse_args()
    with open(args.ours, "rb") as f:
        ours_b = f.read()
    with open(args.ref, "rb") as f:
        ref_b = f.read()
    if len(ours_b) != 128:
        print(f"Expected ours 128 bytes; got {len(ours_b)}", file=sys.stderr)
        return 1
    if len(ref_b) not in (128, 256):
        print(
            f"Expected ref 128 or 256 bytes (64×int16 or 64×int32); got {len(ref_b)}",
            file=sys.stderr,
        )
        return 1
    ours = list(struct.unpack("<64h", ours_b))
    if len(ref_b) == 256:
        ref = list(struct.unpack("<64i", ref_b))
        for i in range(64):
            if ref[i] < -32768 or ref[i] > 32767:
                print(
                    f"Ref value at index {i} out of int16 range: {ref[i]}",
                    file=sys.stderr,
                )
                return 1
            ref[i] = int(ref[i])
    else:
        ref = list(struct.unpack("<64h", ref_b))
    label = "index" if args.order == "natural" else "zigzag index"
    for i in range(64):
        if ours[i] != ref[i]:
            print(
                f"First difference at {label} {i}: ours={ours[i]} ref={ref[i]}",
                file=sys.stderr,
            )
            if i == 0:
                print(f"  DC (index 0): ours={ours[0]} ref={ref[0]}", file=sys.stderr)
            return 1
    print("Match: 64 values identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
