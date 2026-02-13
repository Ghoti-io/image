#!/usr/bin/env python3
"""
Compare our decoder's final coefficient hash to libjpeg-turbo reference.

Runs our decoder with DUMP_JPEG_COEF_AFTER_SCAN=1, parses the last scan hash,
runs dump_jpeg_coef_ref (must be built: cc -o dump_jpeg_coef_ref dump_jpeg_coef_ref.c
$(pkg-config --cflags --libs libjpeg)), and compares.

Exit 0 if hashes match, 1 if they differ, 2 if setup failed (missing ref tool
or decoder, or not a multi-scan JPEG).

Usage:
  python3 compare_coef_hash.py <path-to.jpeg> [path-to-dump_jpeg_raster] [path-to-dump_jpeg_coef_ref]
"""
import os
import re
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "Usage: python3 compare_coef_hash.py <path-to.jpeg> [dump_jpeg_raster] [dump_jpeg_coef_ref]",
            file=sys.stderr,
        )
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    decoder = sys.argv[2] if len(sys.argv) >= 3 else "dump_jpeg_raster"
    ref_tool = sys.argv[3] if len(sys.argv) >= 4 else os.path.join(
        script_dir, "dump_jpeg_coef_ref"
    )
    if not os.path.isabs(ref_tool):
        ref_tool = os.path.join(script_dir, os.path.basename(ref_tool))
    ref_tool = os.path.abspath(ref_tool)

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(decoder) and decoder == "dump_jpeg_raster":
        pass
    elif not os.path.isfile(decoder):
        print(f"Decoder not found: {decoder}", file=sys.stderr)
        return 2
    if not os.path.isfile(ref_tool):
        print(
            f"Reference tool not found: {ref_tool}. Build with:\n"
            "  cc -o dump_jpeg_coef_ref dump_jpeg_coef_ref.c $(pkg-config --cflags --libs libjpeg)",
            file=sys.stderr,
        )
        return 2

    env = os.environ.copy()
    env["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    r = subprocess.run(
        [decoder, jpeg_path],
        capture_output=True,
        timeout=10,
        env=env,
    )
    stderr = (r.stderr or b"").decode("utf-8", errors="replace")
    our_hashes = re.findall(
        r"DUMP_JPEG_COEF_AFTER_SCAN scan(\d+) hash=0x([0-9a-fA-F]+)", stderr
    )
    if not our_hashes:
        print("Our decoder produced no DUMP_JPEG_COEF_AFTER_SCAN lines", file=sys.stderr)
        return 2
    last_scan, our_hash_hex = our_hashes[-1]
    our_hash = int(our_hash_hex, 16)

    r2 = subprocess.run(
        [ref_tool, jpeg_path],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if r2.returncode != 0:
        print(f"Reference tool failed: {r2.stderr or r2.returncode}", file=sys.stderr)
        return 2
    m = re.search(r"COEF_HASH 0x([0-9a-fA-F]+)", r2.stdout)
    if not m:
        print("Reference tool produced no COEF_HASH", file=sys.stderr)
        return 2
    ref_hash = int(m.group(1), 16)

    if our_hash == ref_hash:
        print(f"Match: final coefficient hash 0x{our_hash:x} (scan {last_scan})")
        return 0
    print(
        f"Mismatch: our final (scan {last_scan}) 0x{our_hash:x}, reference 0x{ref_hash:x}",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
