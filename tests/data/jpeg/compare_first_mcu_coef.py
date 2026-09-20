#!/usr/bin/env python3
"""
Compare our decoder's first-MCU coefficients to libjpeg-turbo (dump_jpeg_coef_ref).

Runs our decoder with DUMP_JPEG_COEF_FIRST_MCU=1 and the ref with DUMP_FIRST_MCU=1,
parses both outputs, and reports differing coefficients (comp, block, natural index, ours, ref).

Use this to pinpoint which coefficient(s) are wrong when compare_coef_hash.py
reports a mismatch (e.g. progressive_sample.jpg). Then inspect which scan sets
that band (DC vs AC initial vs refinement).

Usage:
  LD_LIBRARY_PATH=<path> python3 compare_first_mcu_coef.py <file.jpg> [dump_jpeg_raster] [dump_jpeg_coef_ref]
  python3 compare_first_mcu_coef.py --bisect <file.jpg> [dump_jpeg_raster] [dump_jpeg_coef_ref]

  --bisect: run our decoder with GIMG_JPEG_PROGRESSIVE_MAX_SCANS=1,2,...,10, report match/diff
            count per K vs ref (full decode). Identifies first scan where first-MCU diverges.
  In-tree: set LD_LIBRARY_PATH=build/linux/release/apps (or equivalent) so the decoder loads.
Exit 0 match, 1 diff, 2 setup failed.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_path import BUILD_HINT, find_oracle
from typing import Dict, List, Tuple


def parse_our_lines(stderr: str) -> Dict[Tuple[int, int], List[int]]:
    """Parse OUR_COMPc_BLOCKb v0 v1 ... into {(c,b): [64 values]}."""
    out = {}
    for m in re.finditer(r"OUR_COMP(\d+)_BLOCK(\d+) (.+)", stderr):
        c, b = int(m.group(1)), int(m.group(2))
        vals = [int(x) for x in m.group(3).split()]
        if len(vals) == 64:
            out[(c, b)] = vals
    return out


def parse_ref_lines(stdout: str) -> Dict[Tuple[int, int], List[int]]:
    """Parse REF_COMPc_BLOCKb v0 v1 ... into {(c,b): [64 values]}."""
    out = {}
    for line in stdout.splitlines():
        m = re.match(r"REF_COMP(\d+)_BLOCK(\d+) (.+)", line)
        if m:
            c, b = int(m.group(1)), int(m.group(2))
            vals = [int(x) for x in m.group(3).split()]
            if len(vals) == 64:
                out[(c, b)] = vals
    return out


def main() -> int:
    argv = sys.argv[1:]
    bisect = False
    if argv and argv[0] == "--bisect":
        bisect = True
        argv = argv[1:]
    if len(argv) < 1:
        print(
            "Usage: python3 compare_first_mcu_coef.py [--bisect] <file.jpg> [dump_jpeg_raster] [dump_jpeg_coef_ref]",
            file=sys.stderr,
        )
        return 2
    jpeg_path = os.path.abspath(argv[0])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    ref_tool = find_oracle("dump_jpeg_coef_ref", argv[2] if len(argv) >= 3 else None)

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(decoder) and decoder == "dump_jpeg_raster":
        pass
    elif not os.path.isfile(decoder):
        print(f"Decoder not found: {decoder}", file=sys.stderr)
        return 2
    if ref_tool is None:
        print(f"Reference tool not found. {BUILD_HINT}", file=sys.stderr)
        return 2

    env_ref = os.environ.copy()
    env_ref["DUMP_FIRST_MCU"] = "1"
    r2 = subprocess.run(
        [ref_tool, jpeg_path],
        capture_output=True,
        text=True,
        timeout=10,
        env=env_ref,
    )
    if r2.returncode != 0:
        print(f"Reference tool failed: {r2.stderr or r2.returncode}", file=sys.stderr)
        return 2
    ref = parse_ref_lines(r2.stdout)
    if not ref:
        print("Reference tool produced no REF_COMP*_BLOCK* lines", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["DUMP_JPEG_COEF_FIRST_MCU"] = "1"
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    if bisect:
        comp_names = ["Y", "Cb", "Cr"]
        keys_ref = set(ref)
        print("Bisect: first-MCU diff count vs ref (full decode) after K scans:")
        for max_scans in range(1, 11):
            env["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scans)
            r = subprocess.run(
                [decoder, jpeg_path],
                capture_output=True,
                timeout=10,
                env=env,
            )
            our = parse_our_lines((r.stderr or b"").decode("utf-8", errors="replace"))
            if not our:
                print(f"  MAX_SCANS={max_scans}: no dump", file=sys.stderr)
                continue
            keys_our = set(our)
            if keys_our != keys_ref:
                print(f"  MAX_SCANS={max_scans}: block set mismatch", file=sys.stderr)
                continue
            ndiff = 0
            for (c, b) in sorted(keys_our):
                for i in range(64):
                    if our[(c, b)][i] != ref[(c, b)][i]:
                        ndiff += 1
            print(f"  MAX_SCANS={max_scans}: {ndiff} diff(s)")
        return 0

    r = subprocess.run(
        [decoder, jpeg_path],
        capture_output=True,
        timeout=10,
        env=env,
    )
    our = parse_our_lines((r.stderr or b"").decode("utf-8", errors="replace"))
    if not our:
        print("Our decoder produced no DUMP_JPEG_COEF_FIRST_MCU lines", file=sys.stderr)
        return 2

    comp_names = ["Y", "Cb", "Cr"]
    keys_our = set(our)
    keys_ref = set(ref)
    if keys_our != keys_ref:
        print(
            f"Block set mismatch: ours {sorted(keys_our)}, ref {sorted(keys_ref)}",
            file=sys.stderr,
        )
        return 1
    diffs = []
    for (c, b) in sorted(keys_our):
        name = comp_names[c] if c < 3 else f"C{c}"
        for i in range(64):
            if our[(c, b)][i] != ref[(c, b)][i]:
                diffs.append((c, b, name, i, our[(c, b)][i], ref[(c, b)][i]))
    if not diffs:
        print("First MCU coefficients match reference.")
        return 0
    print(f"First MCU: {len(diffs)} coefficient(s) differ:", file=sys.stderr)
    for c, b, name, i, ov, rv in diffs[:50]:
        print(f"  {name}{b} nat[{i}]: ours={ov} ref={rv}", file=sys.stderr)
    if len(diffs) > 50:
        print(f"  ... and {len(diffs) - 50} more", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
