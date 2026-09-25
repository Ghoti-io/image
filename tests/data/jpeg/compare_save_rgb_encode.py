#!/usr/bin/env python3
"""
Compare our encoder's 8×8 baseline RGB output to libjpeg's with the same settings.

Same image as SaveRgbThenLoadDecode: 8×8 RGBA, R=x*32, G=y*32, B=128, A=255.
Settings: baseline, quality 85, 4:2:0.

Steps:
  1. Run JpegEncode.SaveRgbThenLoadDecode with GIMG_JPEG_DUMP_SCAN_BASELINE set
     → produces our scan bytes and tests/out/jpeg/baseline_rgb.jpg
  2. Run encode_libjpeg_baseline_rgb → libjpeg scan bytes + optional full JPEG
  3. Compare scan bytes byte-by-byte; optionally compare full JPEG structure

Run from image/: python3 tests/data/jpeg/compare_save_rgb_encode.py [--work-dir ...]
Requires: make oracle-build oracle-tools, make test (testJpeg_encode).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IMAGE_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
# Oracle tools live in build/.../apps; override with GIMG_JPEG_ORACLE_DIR.
ORACLE_DIR = os.environ.get("GIMG_JPEG_ORACLE_DIR") or SCRIPT_DIR


def jpeg_structure_summary(path: str) -> list[tuple[str, int]]:
    """Return list of (marker_hex, segment_length) for low-level comparison."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        return [("SOI?", 0)]
    out = []
    i = 2
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker == 0xD9:  # EOI
            out.append(("EOI", 0))
            break
        if marker == 0x00 or (0xD0 <= marker <= 0xD7):  # stuffed 0xFF, RST
            out.append((f"0x{marker:02X}", 0))
            continue
        if i + 2 > len(data):
            break
        length = (data[i] << 8) | data[i + 1]
        i += 2
        name = {
            0xC0: "SOF0", 0xC1: "SOF1", 0xC2: "SOF2",
            0xC4: "DHT", 0xDA: "SOS", 0xDB: "DQT", 0xDD: "DRI",
        }.get(marker, f"0x{marker:02X}")
        out.append((name, length))
        i += length - 2  # payload only; length value includes the 2 length bytes
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare our 8×8 baseline RGB encode to libjpeg (same settings)"
    )
    ap.add_argument(
        "--work-dir",
        default=None,
        help="Directory for scan dumps and oracle JPEG (default: tests/out/jpeg)",
    )
    ap.add_argument("--test-exe", default="", help="Path to testJpeg_encode")
    ap.add_argument("--encode-ref", default="", help="Path to encode_libjpeg_baseline_rgb")
    ap.add_argument("--structure", action="store_true", help="Print JPEG marker structure of both files")
    args = ap.parse_args()

    work = args.work_dir or os.path.join(IMAGE_DIR, "tests", "out", "jpeg")
    os.makedirs(work, exist_ok=True)

    encode_ref = args.encode_ref or os.path.join(ORACLE_DIR, "encode_libjpeg_baseline_rgb")
    if not os.path.isfile(encode_ref):
        encode_ref = os.path.join(ORACLE_DIR, "encode_libjpeg_baseline_rgb.exe")
    if not os.path.isfile(encode_ref):
        print(f"ERROR: encode_libjpeg_baseline_rgb not found at {encode_ref}", file=sys.stderr)
        return 1

    test_exe = args.test_exe
    if not test_exe:
        for base in [
            os.path.join(IMAGE_DIR, "build", "linux", "release", "apps"),
            os.path.join(IMAGE_DIR, "build", "mac", "release", "apps"),
            os.path.join(IMAGE_DIR, "build", "win32", "release", "apps"),
        ]:
            p = os.path.join(base, "testJpeg_encode")
            if os.path.isfile(p):
                test_exe = p
                break
            if os.path.isfile(p + ".exe"):
                test_exe = p + ".exe"
                break
    if not test_exe or not os.path.isfile(test_exe):
        print("ERROR: testJpeg_encode not found. Run from image/ and build with make test.", file=sys.stderr)
        return 1

    app_dir = os.path.dirname(test_exe)
    env = os.environ.copy()
    env["GIMG_IMAGE_ROOT"] = IMAGE_DIR
    if app_dir:
        env["LD_LIBRARY_PATH"] = app_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")

    ours_scan = os.path.join(work, "encoder_rgb_ours_scan.bin")
    ref_scan = os.path.join(work, "encoder_rgb_ref_scan.bin")
    oracle_jpeg = os.path.join(work, "oracle_baseline_rgb.jpg")

    # 1) Our encoder: run SaveRgbThenLoadDecode with scan dump
    env_dump = {**env, "GIMG_JPEG_DUMP_SCAN_BASELINE": ours_scan}
    r = subprocess.run(
        [test_exe, "--gtest_filter=JpegEncode.SaveRgbThenLoadDecode"],
        cwd=IMAGE_DIR,
        env=env_dump,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if r.returncode != 0:
        print("SaveRgbThenLoadDecode failed:", r.stderr or r.stdout, file=sys.stderr)
    if not os.path.isfile(ours_scan):
        print("Our encoder did not produce scan dump. Set GIMG_JPEG_DUMP_SCAN_BASELINE.", file=sys.stderr)
        return 1
    ours_jpeg = os.path.join(work, "baseline_rgb.jpg")
    if not os.path.isfile(ours_jpeg):
        ours_jpeg = os.path.join(IMAGE_DIR, "tests", "out", "jpeg", "baseline_rgb.jpg")

    # 2) Libjpeg oracle: same 8×8 RGB, Q85, 4:2:0
    subprocess.run(
        [encode_ref, ref_scan, oracle_jpeg],
        cwd=SCRIPT_DIR,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if not os.path.isfile(ref_scan):
        print("encode_libjpeg_baseline_rgb did not produce ref scan.", file=sys.stderr)
        return 1

    # 3) Compare scan bytes
    with open(ours_scan, "rb") as f:
        ours_bytes = f.read()
    with open(ref_scan, "rb") as f:
        ref_bytes = f.read()
    n = min(len(ours_bytes), len(ref_bytes))
    match = ours_bytes == ref_bytes
    if not match:
        for i in range(n):
            if ours_bytes[i] != ref_bytes[i]:
                print(f"First difference at scan byte {i}: ours=0x{ours_bytes[i]:02x} ref=0x{ref_bytes[i]:02x}")
                start = max(0, i - 8)
                end = min(len(ours_bytes), len(ref_bytes), i + 9)
                print("  ours:", " ".join(f"{ours_bytes[j]:02x}" for j in range(start, end)))
                print("  ref :", " ".join(f"{ref_bytes[j]:02x}" for j in range(start, end)))
                break
        else:
            print(f"Length mismatch: ours={len(ours_bytes)} ref={len(ref_bytes)} (first {n} bytes match)")
    else:
        print(f"Scan bytes match: {len(ours_bytes)} bytes")

    # 4) Optional: JPEG structure comparison
    if args.structure and os.path.isfile(ours_jpeg) and os.path.isfile(oracle_jpeg):
        print("\nJPEG structure (marker, segment length):")
        print("  Ours (baseline_rgb.jpg):", jpeg_structure_summary(ours_jpeg))
        print("  Ref (oracle_baseline_rgb.jpg):", jpeg_structure_summary(oracle_jpeg))

    return 0 if match else 1


if __name__ == "__main__":
    sys.exit(main())
