#!/usr/bin/env python3
"""
Compare first-MCU Cb/Cr quantized block dumps (64 int16_t zigzag order, LE).
Usage: python3 compare_coef_blocks.py <ours_cb> <ref_cb> [ours_cr] [ref_cr]
Reports first differing zigzag index and values; exits 0 if all match.
"""
import struct
import sys


def read_coef_block(path: str) -> list[int]:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != 128:  # 64 * 2
        raise SystemExit(f"{path}: expected 128 bytes, got {len(data)}")
    return list(struct.unpack("<64h", data))


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: compare_coef_blocks.py <ours_cb.bin> <ref_cb.bin> [ours_cr.bin] [ref_cr.bin]",
              file=sys.stderr)
        return 1
    ours_cb = sys.argv[1]
    ref_cb = sys.argv[2]
    ours_cr = sys.argv[3] if len(sys.argv) > 3 else None
    ref_cr = sys.argv[4] if len(sys.argv) > 4 else None

    exit_code = 0
    for name, ours_path, ref_path in [
        ("Cb", ours_cb, ref_cb),
        ("Cr", ours_cr, ref_cr) if (ours_cr and ref_cr) else (None, None, None),
    ]:
        if name is None:
            break
        try:
            ours = read_coef_block(ours_path)
            ref = read_coef_block(ref_path)
        except FileNotFoundError as e:
            print(f"Skip {name}: {e}", file=sys.stderr)
            continue
        for z in range(64):
            if ours[z] != ref[z]:
                print(f"{name} first diff at zigzag[z={z}]: ours={ours[z]} ref={ref[z]}")
                exit_code = 1
                break
        else:
            print(f"{name}: 64 coefficients match")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
