#!/usr/bin/env python3
"""
Run verification: first AC refinement scan first block (script vs decoder TRACE).

Runs verify_ac_refine_first_block.py and the decoder with TRACE_JPEG_AC_REFINE=1
and GIMG_JPEG_PROGRESSIVE_MAX_SCANS set so the first AC refinement scan runs,
then compares TRACE_JPEG_AC_REFINE lines (k= and EOB). Exit 0=match, 1=mismatch.
Usage: python3 run_ac_refine_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]
"""
import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "Usage: python3 run_ac_refine_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]",
            file=sys.stderr,
        )
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    decoder = sys.argv[2] if len(sys.argv) >= 3 else None
    verify_script = os.path.join(script_dir, "verify_ac_refine_first_block.py")

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(verify_script):
        print(f"Script not found: {verify_script}", file=sys.stderr)
        return 2

    decoder_abs = os.path.abspath(decoder) if decoder and os.path.isfile(decoder) else None
    if decoder and not decoder_abs:
        decoder_abs = os.path.abspath(os.path.join(os.path.dirname(script_dir), "..", decoder))
    if decoder_abs and os.path.isfile(decoder_abs):
        app_dir = os.path.dirname(decoder_abs)
        run_env = os.environ.copy()
        run_env["LD_LIBRARY_PATH"] = app_dir + os.pathsep + run_env.get("LD_LIBRARY_PATH", "")
    else:
        run_env = os.environ.copy()
        decoder_abs = None

    r = subprocess.run(
        [sys.executable, verify_script, jpeg_path]
        + ([decoder_abs] if decoder_abs else []),
        capture_output=True,
        text=True,
        timeout=15,
        cwd=script_dir,
        env=run_env,
    )
    if r.returncode != 0:
        print(f"verify_ac_refine_first_block.py failed: {r.stderr or r.returncode}", file=sys.stderr)
        return 2
    script_lines = [
        line.strip()
        for line in r.stdout.splitlines()
        if line.strip().startswith("TRACE_JPEG_AC_REFINE ")
    ]

    # Run decoder only through first AC refinement scan so TRACE matches script.
    env = run_env.copy()
    env["TRACE_JPEG_AC_REFINE"] = "1"
    env["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = "4"
    decoder_cmd = [decoder_abs or os.path.join(script_dir, "dump_jpeg_raster"), jpeg_path]
    if not decoder_abs and decoder:
        decoder_cmd = [os.path.abspath(decoder), jpeg_path]
    r2 = subprocess.run(
        decoder_cmd,
        capture_output=True,
        timeout=10,
        env=env,
    )
    stderr = (r2.stderr or b"").decode("utf-8", errors="replace")
    decoder_lines = [
        line.strip()
        for line in stderr.splitlines()
        if "TRACE_JPEG_AC_REFINE" in line
        and " before_read " not in line
        and (" k=" in line or " EOB" in line)
    ]

    if script_lines != decoder_lines:
        print("Mismatch:", file=sys.stderr)
        for i, (a, b) in enumerate(zip(script_lines, decoder_lines)):
            if a != b:
                print(f"  script:  {a}", file=sys.stderr)
                print(f"  decoder: {b}", file=sys.stderr)
        if len(script_lines) != len(decoder_lines):
            print(
                f"  script has {len(script_lines)} lines, decoder has {len(decoder_lines)}",
                file=sys.stderr,
            )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
