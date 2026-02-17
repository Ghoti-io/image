#!/usr/bin/env python3
"""
Compare our encoder's baseline scan bytes to libjpeg's (oracle), byte-by-byte.

Prerequisites:
  1. make jpeg-oracle-tools   # builds encode_libjpeg_baseline_scan
  2. Run our encoder with GIMG_JPEG_DUMP_SCAN_BASELINE=<ours.bin> so it writes
     scan bytes to ours.bin (e.g. run the Large640x480BaselineGrayscale test
     with that env set, after ensuring the test writes the JPEG and we have
     a way to trigger the dump - the dump happens on save).

Usage:
  # Step 1: Run our encoder and dump scan (env var must be set when saving).
  GIMG_JPEG_DUMP_SCAN_BASELINE=/tmp/ours_scan.bin \
    ./build/.../testJpeg_encode --gtest_filter=JpegEncode.Large640x480BaselineGrayscale
  # Step 2: Run libjpeg encoder with same params (640x480, quality 85).
  ./tests/data/jpeg/encode_libjpeg_baseline_scan 640 480 85 /tmp/ref_scan.bin
  # Step 3: Compare.
  python3 compare_baseline_scan_oracle.py /tmp/ours_scan.bin /tmp/ref_scan.bin

Or run this script with --run-test to run the test with dump, then encode_libjpeg,
then compare (requires paths to test binary and encode_libjpeg_baseline_scan).
"""
import argparse
import os
import subprocess
import sys


def compare_files(ours_path: str, ref_path: str) -> int:
    with open(ours_path, "rb") as f:
        ours = f.read()
    with open(ref_path, "rb") as f:
        ref = f.read()
    n = min(len(ours), len(ref))
    for i in range(n):
        if ours[i] != ref[i]:
            print(f"First difference at byte {i}: ours=0x{ours[i]:02x} ref=0x{ref[i]:02x}")
            # Show context
            start = max(0, i - 8)
            end = min(len(ours), len(ref), i + 9)
            print("  ours:", " ".join(f"{ours[j]:02x}" for j in range(start, end)))
            print("  ref :", " ".join(f"{ref[j]:02x}" for j in range(start, end)))
            return 1
    if len(ours) != len(ref):
        print(f"Length mismatch: ours={len(ours)} ref={len(ref)} (match first {n} bytes)")
        return 1
    print(f"Match: {len(ours)} bytes")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare our baseline scan bytes to libjpeg")
    ap.add_argument("ours", nargs="?", help="Path to our encoder scan dump")
    ap.add_argument("ref", nargs="?", help="Path to libjpeg encoder scan dump")
    ap.add_argument("--run-test", action="store_true", help="Run test with dump, then libjpeg encode, then compare")
    ap.add_argument("--test-exe", default="", help="Path to testJpeg_encode (for --run-test)")
    ap.add_argument("--encode-ref", default="", help="Path to encode_libjpeg_baseline_scan (for --run-test)")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--quality", type=int, default=85)
    args = ap.parse_args()

    if args.run_test:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        test_exe = args.test_exe or os.path.join(script_dir, "../../../build/linux/release/apps/testJpeg_encode")
        encode_ref = args.encode_ref or os.path.join(script_dir, "encode_libjpeg_baseline_scan")
        if not os.path.isfile(encode_ref):
            encode_ref = os.path.join(script_dir, "encode_libjpeg_baseline_scan.exe")
        ours_path = os.path.join(script_dir, "trace_ours_scan.bin")
        ref_path = os.path.join(script_dir, "trace_ref_scan.bin")
        env = os.environ.copy()
        env["GIMG_JPEG_DUMP_SCAN_BASELINE"] = ours_path
        r = subprocess.run(
            [test_exe, "--gtest_filter=JpegEncode.Large640x480BaselineGrayscale"],
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
        )
        if r.returncode != 0:
            print("Test failed (expected for current encoder); checking for scan dump...", file=sys.stderr)
        if not os.path.isfile(ours_path):
            print(f"Scan dump not found at {ours_path}. Ensure GIMG_JPEG_DUMP_SCAN_BASELINE was set.", file=sys.stderr)
            return 2
        subprocess.run(
            [encode_ref, str(args.width), str(args.height), str(args.quality), ref_path],
            check=True,
            cwd=script_dir,
        )
        return compare_files(ours_path, ref_path)

    if not args.ours or not args.ref:
        ap.print_help()
        return 2
    return compare_files(args.ours, args.ref)


if __name__ == "__main__":
    sys.exit(main())
