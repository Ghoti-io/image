#!/usr/bin/env python3
"""
Compare decoded pixels: libjpeg (dump_jpeg_pixels_ref) vs our decoder (dump_jpeg_raster).

Reports actual pixel values at first difference and summary stats (max diff,
count of differing pixels) so we can see how far off we are and iterate.
Uses libjpeg as the spec-aligned oracle (same as DecodeProgressivePillowOracle).

Usage:
  python3 compare_progressive_pixels.py <path-to.jpeg> [path-to-dump_jpeg_raster] [path-to-dump_jpeg_pixels_ref]

  Optional: set DUMP_OURS_RAW=path and DUMP_REF_RAW=path to keep the raw dumps for inspection.

Exit 0: pixels match. Exit 1: mismatch (or decoder/ref failed).

With --verbose, print the first row of ref and ours (R,G,B per pixel) so you can
see exact values and how far off we are.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_path import BUILD_HINT, find_oracle


def load_ref_raw(path: str) -> tuple[bytes, int, int, int]:
    """Load ref .raw from dump_jpeg_pixels_ref -o. Returns (pixels, w, h, mode).
    Header: 1 byte mode (0=L, 1=RGB, 2=CMYK), 4 bytes w LE, 4 bytes h LE.
    Then L: w*h bytes; RGB: w*h*3; CMYK: w*h*4.
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 9:
        raise ValueError(f"{path}: too short for header")
    mode = data[0]
    w = struct.unpack("<I", data[1:5])[0]
    h = struct.unpack("<I", data[5:9])[0]
    if mode == 0:
        expected = 9 + w * h
    elif mode == 1:
        expected = 9 + w * h * 3
    elif mode == 2:
        expected = 9 + w * h * 4
    else:
        raise ValueError(f"{path}: unknown mode {mode}")
    if len(data) != expected:
        raise ValueError(f"{path}: expected {expected} bytes, got {len(data)}")
    return data[9:], w, h, mode


def load_ours_raw(path: str) -> tuple[bytes, int, int]:
    """Load our decoder stdout: 4 bytes w LE, 4 bytes h LE, then RGBA (w*h*4).
    GRAY8 is expanded to RGBA in dump_jpeg_raster.
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8:
        raise ValueError(f"{path}: too short for dims")
    w, h = struct.unpack("<II", data[:8])
    expected = 8 + w * h * 4
    if len(data) != expected:
        raise ValueError(f"{path}: expected {expected} bytes, got {len(data)}")
    return data[8:], w, h


def compare_pixels(
    ref_pixels: bytes,
    ref_w: int,
    ref_h: int,
    ref_mode: int,
    ours_pixels: bytes,
    ours_w: int,
    ours_h: int,
) -> tuple[bool, str, int, int, int]:
    """Compare ref vs ours. Return (match, message, first_x, first_y, diff_count).
    ref_mode 0=L, 1=RGB, 2=CMYK. Ours is always RGBA (4 bytes per pixel).
    """
    if ref_w != ours_w or ref_h != ours_h:
        return False, f"dimension mismatch: ref {ref_w}x{ref_h} vs ours {ours_w}x{ours_h}", 0, 0, 0

    diff_count = 0
    first_x, first_y = -1, -1
    ref_vals: list[int] = []
    ours_vals: list[int] = []
    max_diff = 0

    for y in range(ref_h):
        for x in range(ref_w):
            if ref_mode == 0:
                ref_off = y * ref_w + x
                r_ref = g_ref = b_ref = ref_pixels[ref_off]
            elif ref_mode == 1:
                ref_off = (y * ref_w + x) * 3
                r_ref = ref_pixels[ref_off]
                g_ref = ref_pixels[ref_off + 1]
                b_ref = ref_pixels[ref_off + 2]
            else:
                ref_off = (y * ref_w + x) * 4
                r_ref = ref_pixels[ref_off]
                g_ref = ref_pixels[ref_off + 1]
                b_ref = ref_pixels[ref_off + 2]
                # CMYK: we compare C,M,Y; K or full 4 if needed
            # load_ours_raw returns pixels only (no 8-byte header), so offset = (y*w+x)*4
            ours_off = (y * ours_w + x) * 4
            r_ours = ours_pixels[ours_off]
            g_ours = ours_pixels[ours_off + 1]
            b_ours = ours_pixels[ours_off + 2]

            dr = abs(int(r_ours) - int(r_ref))
            dg = abs(int(g_ours) - int(g_ref))
            db = abs(int(b_ours) - int(b_ref))
            d = max(dr, dg, db)
            if d > 0:
                diff_count += 1
                if d > max_diff:
                    max_diff = d
                if first_x < 0:
                    first_x, first_y = x, y
                    ref_vals = [r_ref, g_ref, b_ref]
                    ours_vals = [r_ours, g_ours, b_ours]

    if diff_count == 0:
        return True, "Pixels match.", 0, 0, 0

    msg = (
        f"First diff at ({first_x},{first_y}): "
        f"ref=({ref_vals[0]},{ref_vals[1]},{ref_vals[2]}) "
        f"ours=({ours_vals[0]},{ours_vals[1]},{ours_vals[2]}) "
        f"|diff|=({abs(ours_vals[0]-ref_vals[0])},{abs(ours_vals[1]-ref_vals[1])},{abs(ours_vals[2]-ref_vals[2])}). "
        f"Total differing pixels: {diff_count}/{ref_w*ref_h}, max_abs_diff={max_diff}."
    )
    return False, msg, first_x, first_y, diff_count


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare decoded pixels: libjpeg ref vs our decoder (actual values, not just hash)."
    )
    parser.add_argument("jpeg", help="Path to JPEG file")
    parser.add_argument("our_decoder", nargs="?", default="dump_jpeg_raster", help="Path to dump_jpeg_raster")
    parser.add_argument("ref_decoder", nargs="?", default=None, help="Path to dump_jpeg_pixels_ref")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print first row ref vs ours (R,G,B)")
    args = parser.parse_args()

    jpeg_path = os.path.abspath(args.jpeg)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    our_decoder = args.our_decoder
    ref_decoder = find_oracle("dump_jpeg_pixels_ref", args.ref_decoder)
    if ref_decoder is None:
        print(f"Reference tool not found. {BUILD_HINT}", file=sys.stderr)
        return 1
    if os.path.isfile(ref_decoder):
        ref_decoder = os.path.abspath(ref_decoder)
    if our_decoder != "dump_jpeg_raster" and os.path.isfile(our_decoder):
        our_decoder = os.path.abspath(our_decoder)

    if not os.path.isfile(jpeg_path):
        print(f"Not a file: {jpeg_path}", file=sys.stderr)
        return 1

    keep_ref = os.environ.get("DUMP_REF_RAW")
    keep_ours = os.environ.get("DUMP_OURS_RAW")
    tmp_dir = None
    if not keep_ref or not keep_ours:
        import tempfile
        tmp_dir = tempfile.mkdtemp(prefix="compare_pixels_")
        ref_raw = keep_ref or os.path.join(tmp_dir, "ref.raw")
        ours_raw = keep_ours or os.path.join(tmp_dir, "ours.raw")
    else:
        ref_raw = keep_ref
        ours_raw = keep_ours

    env = os.environ.copy()
    env_ours = env.copy()
    if script_dir not in env_ours.get("LD_LIBRARY_PATH", ""):
        lib_path = os.path.join(os.path.dirname(script_dir), "..", "..", "build", "linux", "release", "apps")
        if os.path.isdir(lib_path):
            env_ours["LD_LIBRARY_PATH"] = lib_path + os.pathsep + env_ours.get("LD_LIBRARY_PATH", "")

    # Run ref: dump_jpeg_pixels_ref -o ref.raw jpeg
    r_ref = subprocess.run(
        [ref_decoder, "-o", ref_raw, jpeg_path],
        capture_output=True,
        env=env,
        cwd=script_dir,
    )
    if r_ref.returncode != 0:
        print(f"dump_jpeg_pixels_ref failed: {r_ref.stderr.decode(errors='replace')}", file=sys.stderr)
        if tmp_dir and os.path.isdir(tmp_dir):
            import shutil
            shutil.rmtree(tmp_dir, ignore_errors=True)
        return 1

    # Run ours: dump_jpeg_raster -o ours.raw jpeg (or stdout if no -o)
    cmd_ours = [our_decoder, jpeg_path]
    out_ours = None
    if os.path.isfile(our_decoder):
        # Prefer -o if supported
        r_ours = subprocess.run(
            [our_decoder, "-o", ours_raw, jpeg_path],
            capture_output=True,
            env=env_ours,
            cwd=os.path.dirname(jpeg_path),
        )
        if r_ours.returncode != 0:
            # Fallback: no -o, capture stdout
            r_ours = subprocess.run(
                [our_decoder, jpeg_path],
                capture_output=True,
                env=env_ours,
                cwd=os.path.dirname(jpeg_path),
            )
            if r_ours.returncode != 0:
                print(f"dump_jpeg_raster failed: {r_ours.stderr.decode(errors='replace')}", file=sys.stderr)
                if tmp_dir and os.path.isdir(tmp_dir):
                    import shutil
                    shutil.rmtree(tmp_dir, ignore_errors=True)
                return 1
            out_ours = r_ours.stdout
            with open(ours_raw, "wb") as f:
                f.write(out_ours)
        else:
            out_ours = open(ours_raw, "rb").read()
    else:
        r_ours = subprocess.run(
            [our_decoder, jpeg_path],
            capture_output=True,
            env=env_ours,
            cwd=os.path.dirname(jpeg_path),
        )
        if r_ours.returncode != 0:
            print(f"dump_jpeg_raster failed: {r_ours.stderr.decode(errors='replace')}", file=sys.stderr)
            if tmp_dir and os.path.isdir(tmp_dir):
                import shutil
                shutil.rmtree(tmp_dir, ignore_errors=True)
            return 1
        out_ours = r_ours.stdout
        with open(ours_raw, "wb") as f:
            f.write(out_ours)

    try:
        ref_pixels, ref_w, ref_h, ref_mode = load_ref_raw(ref_raw)
    except Exception as e:
        print(f"Failed to load ref raw: {e}", file=sys.stderr)
        if tmp_dir and os.path.isdir(tmp_dir):
            import shutil
            shutil.rmtree(tmp_dir, ignore_errors=True)
        return 1

    try:
        ours_pixels, ours_w, ours_h = load_ours_raw(ours_raw)
    except Exception as e:
        print(f"Failed to load ours raw: {e}", file=sys.stderr)
        if tmp_dir and os.path.isdir(tmp_dir):
            import shutil
            shutil.rmtree(tmp_dir, ignore_errors=True)
        return 1

    match, msg, fx, fy, diff_count = compare_pixels(
        ref_pixels, ref_w, ref_h, ref_mode,
        ours_pixels, ours_w, ours_h,
    )

    if keep_ref or keep_ours:
        if keep_ref:
            print(f"Ref raw: {ref_raw}", file=sys.stderr)
        if keep_ours:
            print(f"Ours raw: {ours_raw}", file=sys.stderr)
    if tmp_dir and os.path.isdir(tmp_dir):
        import shutil
        shutil.rmtree(tmp_dir, ignore_errors=True)

    if match:
        print("Pixels match (libjpeg vs our decoder).")
        return 0

    print(msg, file=sys.stderr)
    print(f"First diff at pixel ({fx},{fy}).", file=sys.stderr)

    if args.verbose and ref_w > 0 and ref_h > 0:
        # Print first row: ref (R,G,B) and ours (R,G,B) for x=0..min(16, ref_w)
        n = min(16, ref_w)
        print("\nFirst row (x=0..%d) ref then ours (R G B per pixel):" % (n - 1), file=sys.stderr)
        ref_row = []
        for x in range(n):
            if ref_mode == 0:
                off = ref_w * 0 + x
                ref_row.append((ref_pixels[off], ref_pixels[off], ref_pixels[off]))
            else:
                off = (ref_w * 0 + x) * 3
                ref_row.append((ref_pixels[off], ref_pixels[off + 1], ref_pixels[off + 2]))
        ours_row = []
        for x in range(n):
            off = (0 * ours_w + x) * 4
            ours_row.append((ours_pixels[off], ours_pixels[off + 1], ours_pixels[off + 2]))
        print("  ref:  " + " ".join("%d,%d,%d" % t for t in ref_row), file=sys.stderr)
        print("  ours: " + " ".join("%d,%d,%d" % t for t in ours_row), file=sys.stderr)
        diffs = [
            (x, ref_row[x], ours_row[x], tuple(abs(ours_row[x][c] - ref_row[x][c]) for c in range(3)))
            for x in range(n)
            if ref_row[x] != ours_row[x]
        ]
        if diffs:
            print("  |diff| at: " + " ".join("x=%d (%d,%d,%d)" % (x, d[0], d[1], d[2]) for x, _, _, d in diffs[:8]), file=sys.stderr)

    return 1


if __name__ == "__main__":
    sys.exit(main())
