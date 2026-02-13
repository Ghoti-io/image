#!/usr/bin/env python3
"""
Run verification: first AC-initial scan first block (script vs decoder TRACE).

Runs verify_first_ac_initial.py and the decoder with TRACE_JPEG_AC_SYMBOLS=1,
then compares. Exit 0=match, 1=mismatch, 2=setup failed.
Usage: python3 run_first_ac_initial_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]
"""
import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: python3 run_first_ac_initial_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]", file=sys.stderr)
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    verify_script = os.path.join(script_dir, "verify_first_ac_initial.py")
    decoder = sys.argv[2] if len(sys.argv) >= 3 else "dump_jpeg_raster"
    if decoder and not os.path.isabs(decoder):
        decoder = os.path.abspath(decoder)

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(verify_script):
        print(f"Script not found: {verify_script}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["TRACE_JPEG_AC_SYMBOLS"] = "1"
    # So decoder can load shared libs when invoked with a path (e.g. from repo root).
    if "LD_LIBRARY_PATH" not in env and decoder and os.path.isfile(decoder):
        decoder_dir = os.path.dirname(decoder)
        # image/build/.../apps often needs ../compress/build/.../apps for libghoti_compress.
        parts = os.path.normpath(decoder).split(os.sep)
        if "image" in parts and "build" in parts and "apps" in parts:
            idx = parts.index("image")
            prefix = os.sep.join(parts[:idx]) if idx else os.getcwd()
            compress_apps = os.path.join(prefix, "compress", "build", "linux", "release", "apps")
            if os.path.isdir(compress_apps):
                env["LD_LIBRARY_PATH"] = decoder_dir + os.pathsep + compress_apps
            else:
                env["LD_LIBRARY_PATH"] = decoder_dir
        else:
            env["LD_LIBRARY_PATH"] = decoder_dir

    r = subprocess.run(
        [sys.executable, verify_script, jpeg_path],
        capture_output=True,
        text=True,
        timeout=10,
        cwd=script_dir,
    )
    if r.returncode != 0:
        print(f"verify_first_ac_initial.py failed: {r.stderr or r.returncode}", file=sys.stderr)
        return 2
    script_lines = [line.strip() for line in r.stdout.splitlines() if line.strip()]

    jpeg_abs = os.path.abspath(jpeg_path)
    r2 = subprocess.run(
        [decoder, jpeg_abs],
        capture_output=True,
        timeout=10,
        env=env,
    )
    stderr = (r2.stderr or b"").decode("utf-8", errors="replace")
    decoder_lines = [line.strip() for line in stderr.splitlines() if "TRACE_JPEG_AC_SYMBOLS" in line]

    if script_lines != decoder_lines:
        print("Mismatch:", file=sys.stderr)
        for i, (a, b) in enumerate(zip(script_lines, decoder_lines)):
            if a != b:
                print(f"  script:  {a}", file=sys.stderr)
                print(f"  decoder: {b}", file=sys.stderr)
        if len(script_lines) != len(decoder_lines):
            print(f"  script has {len(script_lines)} lines, decoder has {len(decoder_lines)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
