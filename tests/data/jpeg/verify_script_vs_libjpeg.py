#!/usr/bin/env python3
"""
Verify whether the Python reference (compare_blocks_after_scans) matches libjpeg.

Libjpeg is the source of truth (Pillow matches libjpeg pixel hash). This script:
1. Builds the reference block from the bitstream (same logic as compare_blocks).
2. Gets libjpeg's first block (DUMP_FIRST_BLOCK).
3. Compares and reports differences.
4. If our decoder path is given, also compares decoder's final block to libjpeg
   (decoder vs libjpeg). If decoder matches libjpeg but script does not, the
   script is wrong; if both disagree, both may share the same bug.

For 8x8 single-component only. Exits 0 if script matches libjpeg, 1 if not.

Usage:
  python3 verify_script_vs_libjpeg.py <8x8-gray.jpg> [dump_jpeg_coef_ref [our_decoder]]
"""
import os
import re
import subprocess
import sys

script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, script_dir)

# Natural index n -> zigzag index (inverse of jpeg_natural_order). Must match decoder's gimg_jpeg_inv_zigzag.
INV_ZIGZAG = [
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
]


def _zz(n: int) -> int:
    """Natural index n -> zigzag index (for diagnostics)."""
    return INV_ZIGZAG[n]


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: verify_script_vs_libjpeg.py <8x8-gray.jpg> [dump_jpeg_coef_ref [our_decoder]]", file=sys.stderr)
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    ref_tool = sys.argv[2] if len(sys.argv) >= 3 else os.path.join(script_dir, "dump_jpeg_coef_ref")
    our_decoder = sys.argv[3] if len(sys.argv) >= 4 else None
    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(ref_tool):
        print(f"Reference tool not found: {ref_tool}", file=sys.stderr)
        return 2

    with open(jpeg_path, "rb") as f:
        data = bytearray(f.read())

    # Get reference block from script (decode_ref_blocks; final = ref_after_4 for 5 scans)
    from compare_blocks_after_scans import decode_ref_blocks  # type: ignore

    refs = decode_ref_blocks(data)
    if refs is None:
        print("Script could not build reference (need 8x8 single-component, 5+ scans)", file=sys.stderr)
        return 1
    ref_2, ref_3, ref_4 = refs[0], refs[1], refs[2]
    ref_5 = refs[3] if len(refs) > 3 else None
    # Use final block after all scans (e.g. ref_5 for 6-scan file) so we compare to libjpeg's full decode.
    script_final_zig = ref_5 if ref_5 is not None else ref_4
    script_natural = [script_final_zig[INV_ZIGZAG[n]] for n in range(64)]

    env = os.environ.copy()
    env["DUMP_FIRST_BLOCK"] = "1"
    r = subprocess.run(
        [ref_tool, jpeg_path],
        capture_output=True,
        text=True,
        timeout=10,
        env=env,
    )
    if r.returncode != 0:
        print(f"libjpeg ref failed: {r.stderr or r.returncode}", file=sys.stderr)
        return 2
    m = re.search(r"FIRST_BLOCK\s+((?:-?\d+\s+)+)", r.stdout)
    if not m:
        print("libjpeg produced no FIRST_BLOCK", file=sys.stderr)
        return 2
    libjpeg_natural = [int(x) for x in m.group(1).split()]
    if len(libjpeg_natural) != 64:
        print(f"libjpeg block length {len(libjpeg_natural)}", file=sys.stderr)
        return 2

    # Optionally compare our decoder's final block to libjpeg (decoder dumps zigzag).
    if our_decoder and os.path.isfile(our_decoder):
        env_dec = os.environ.copy()
        env_dec["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
        env_dec["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = "10"
        if os.environ.get("LD_LIBRARY_PATH"):
            env_dec["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
        else:
            image_dir = os.path.dirname(os.path.dirname(os.path.dirname(script_dir)))
            apps = os.path.join(image_dir, "build", "linux", "release", "apps")
            if os.path.isdir(apps):
                env_dec["LD_LIBRARY_PATH"] = apps
        r_dec = subprocess.run(
            [our_decoder, jpeg_path],
            capture_output=True,
            timeout=10,
            env=env_dec,
        )
        dec_stderr = (r_dec.stderr or b"").decode("utf-8", errors="replace")
        blocks = list(re.finditer(r"DUMP_JPEG_COEF_AFTER_SCAN scan(\d+) block:\s*((?:-?\d+\s+)+)", dec_stderr))
        if blocks:
            last = blocks[-1]
            dec_zig = [int(x) for x in last.group(2).split()]
            if len(dec_zig) == 64:
                dec_natural = [dec_zig[INV_ZIGZAG[n]] for n in range(64)]
                dec_diffs = [(n, dec_natural[n], libjpeg_natural[n]) for n in range(64) if dec_natural[n] != libjpeg_natural[n]]
                dec_match = 64 - len(dec_diffs)
                print(f"Decoder vs libjpeg (natural order): {dec_match}/64 match")
                if dec_match == 64:
                    print("Decoder matches libjpeg (so script is wrong).")
                elif dec_diffs:
                    print("Decoder also disagrees with libjpeg (script and decoder may share same bug).")

    diffs = [(n, script_natural[n], libjpeg_natural[n]) for n in range(64) if script_natural[n] != libjpeg_natural[n]]
    match_count = 64 - len(diffs)

    # Diagnostic: positions where we have non-zero but ref has zero (extra non-zeros → scan 5 underflow)
    # and where we have zero but ref has non-zero (missing non-zeros).
    ours_nz_ref_zero = [(n, script_natural[n], _zz(n)) for n in range(64) if script_natural[n] != 0 and libjpeg_natural[n] == 0]
    ours_zero_ref_nz = [(n, libjpeg_natural[n], _zz(n)) for n in range(64) if script_natural[n] == 0 and libjpeg_natural[n] != 0]
    if ours_nz_ref_zero or ours_zero_ref_nz:
        print("Non-zero pattern (script vs libjpeg):")
        if ours_nz_ref_zero:
            print(f"  Ours non-zero, ref zero (natural_idx, script_val, zigzag): {[(n, s, zz) for n, s, zz in ours_nz_ref_zero]}")
        if ours_zero_ref_nz:
            print(f"  Ours zero, ref non-zero (natural_idx, ref_val, zigzag): {[(n, l, zz) for n, l, zz in ours_zero_ref_nz]}")

    # Band diagnostic (T.81 Annex G: scan 1 = zigzag 1-5, scan 2 = 6-63; refinement applies to band).
    # DC (zigzag 0) is scan 0/4; differences in 1-5 point to scan 1 or refinement; in 6-63 to scan 2 or refinement.
    if diffs:
        band_1_5 = [(n, s, l, _zz(n)) for n, s, l in diffs if 1 <= _zz(n) <= 5]
        band_6_63 = [(n, s, l, _zz(n)) for n, s, l in diffs if 6 <= _zz(n) <= 63]
        dc_diffs = [(n, s, l) for n, s, l in diffs if _zz(n) == 0]
        print("Diffs by band (zigzag; T.81 progressive bands):")
        print(f"  DC (zigzag 0): {len(dc_diffs)} diffs")
        print(f"  Band 1-5 (scan 1 AC initial): {len(band_1_5)} diffs")
        print(f"  Band 6-63 (scan 2 AC initial + refinement): {len(band_6_63)} diffs")
        if band_1_5:
            mag_diffs_1_5 = [abs(s - l) for _, s, l, _ in band_1_5 if s != 0 or l != 0]
            if mag_diffs_1_5:
                print(f"  Band 1-5 magnitude deltas (sample): {mag_diffs_1_5[:10]}")

    print(f"Script vs libjpeg (natural order): {match_count}/64 match")
    if not diffs:
        print("Script matches libjpeg (script is correct).")
        return 0
    print("Script DISAGREES with libjpeg (script is wrong; libjpeg is source of truth).")
    print("First differences (natural_idx, script, libjpeg):")
    for n, s, l in diffs[:20]:
        print(f"  [{n}] script={s} libjpeg={l}")
    if len(diffs) > 20:
        print(f"  ... and {len(diffs) - 20} more")
    print(f"\nDC (position 0): script={script_natural[0]} libjpeg={libjpeg_natural[0]}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
