#!/usr/bin/env python3
"""
Run verification: second AC-initial scan first block (script vs decoder block).

Runs verify_second_ac_initial_first_block.py and the decoder with
GIMG_JPEG_PROGRESSIVE_MAX_SCANS=3 and DUMP_JPEG_COEF_AFTER_SCAN=1, then compares
the script's 58 coefficients (zigzag 6..63) to the decoder's first block
indices 6..63 after scan 2. Exit 0=match, 1=mismatch, 2=setup failed.

Usage: python3 run_second_ac_initial_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]
"""
import os
import re
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "Usage: python3 run_second_ac_initial_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]",
            file=sys.stderr,
        )
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    verify_script = os.path.join(script_dir, "verify_second_ac_initial_first_block.py")
    decoder = sys.argv[2] if len(sys.argv) >= 3 else "dump_jpeg_raster"

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(verify_script):
        print(f"Script not found: {verify_script}", file=sys.stderr)
        return 2
    if decoder == "dump_jpeg_raster" and not os.path.isfile(decoder):
        # Try build dir
        for candidate in (
            os.path.join(script_dir, "..", "..", "build", "linux", "release", "apps", "dump_jpeg_raster"),
            os.path.join(os.path.dirname(script_dir), "..", "..", "build", "linux", "release", "apps", "dump_jpeg_raster"),
        ):
            if os.path.isfile(candidate):
                decoder = candidate
                break
        else:
            print("dump_jpeg_raster not found; pass path as second arg", file=sys.stderr)
            return 2

    r = subprocess.run(
        [sys.executable, verify_script, jpeg_path],
        capture_output=True,
        text=True,
        timeout=10,
        cwd=script_dir,
    )
    if r.returncode != 0:
        print(f"verify_second_ac_initial_first_block.py failed: {r.stderr or r.returncode}", file=sys.stderr)
        return 2
    lines = [line.strip() for line in r.stdout.splitlines() if line.strip()]
    if not lines or lines[0] != "SCAN2_AC_INITIAL_BLOCK":
        print("Script did not output SCAN2_AC_INITIAL_BLOCK", file=sys.stderr)
        return 2
    script_vals = [int(x) for x in lines[1 : 1 + 58]]
    if len(script_vals) != 58:
        print(f"Script produced {len(script_vals)} values, expected 58", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = "3"
    env["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    app_dir = os.path.dirname(decoder)
    if app_dir:
        env.setdefault("LD_LIBRARY_PATH", "")
        if env["LD_LIBRARY_PATH"]:
            env["LD_LIBRARY_PATH"] = app_dir + os.pathsep + env["LD_LIBRARY_PATH"]
        else:
            env["LD_LIBRARY_PATH"] = app_dir

    r2 = subprocess.run(
        [decoder, jpeg_path],
        capture_output=True,
        timeout=10,
        env=env,
    )
    stderr = (r2.stderr or b"").decode("utf-8", errors="replace")
    # DUMP_JPEG_COEF_AFTER_SCAN scan2 block: v0 v1 ... v63
    m = re.search(r"DUMP_JPEG_COEF_AFTER_SCAN scan2 block:\s*((?:-?\d+\s+)+)", stderr)
    if not m:
        print("Decoder produced no DUMP_JPEG_COEF_AFTER_SCAN scan2 block (need 8x8 gray fixture)", file=sys.stderr)
        return 2
    decoder_block = [int(x) for x in m.group(1).split()]
    if len(decoder_block) != 64:
        print(f"Decoder block has {len(decoder_block)} values, expected 64", file=sys.stderr)
        return 2
    decoder_vals = decoder_block[6:64]

    if script_vals != decoder_vals:
        print("Mismatch (script band 6..63 vs decoder block 6..63):", file=sys.stderr)
        for i in range(58):
            if script_vals[i] != decoder_vals[i]:
                print(f"  zigzag {6 + i}: script={script_vals[i]} decoder={decoder_vals[i]}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
