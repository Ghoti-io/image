#!/usr/bin/env python3
"""
Sanity check: (1) libjpeg baseline vs progressive encoding, (2) do those two
libjpeg encodings decode to the same raster? (3) does our decoder match
libjpeg decode for each?

Uses the same 16×16 grayscale test pattern as ProgressiveWithRefinementScanDecodeMatchesBaseline:
  pixel(x,y) = (x + y*16) & 0xFF

Requires:
  - encode_libjpeg_baseline_scan (make oracle-build oracle-tools) in this dir
  - cjpeg (libjpeg-turbo-progs) for progressive encode
  - dump_jpeg_pixels_ref in this dir
  - dump_jpeg_raster (our decoder) - pass path or set LD_LIBRARY_PATH for build/apps

Usage:
  python3 sanity_check_libjpeg_baseline_progressive.py [--work-dir /tmp/sanity] [--dump_jpeg_raster path] [--dump_jpeg_pixels_ref path] [--encode_baseline path]

Exit 0: all checks pass. Exit 1: tool missing or check failed.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ORACLE_DIR = os.environ.get("GIMG_JPEG_ORACLE_DIR") or SCRIPT_DIR
W = 16
H = 16
QUALITY = 85

# FNV-1a for hash (match dump_jpeg_pixels_ref / raster_pixel_hash)
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFF_FFFFFFFF
    return h


def load_ref_raw(path: str) -> tuple[bytes, int, int, int]:
    """Ref .raw: 1 byte mode, 4 w LE, 4 h LE, then pixels (L: w*h, RGB: w*h*3)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 9:
        raise ValueError(f"{path}: too short")
    mode = data[0]
    w, h = struct.unpack("<II", data[1:9])
    if mode == 0:
        payload = 9 + w * h
    elif mode == 1:
        payload = 9 + w * h * 3
    else:
        raise ValueError(f"{path}: unsupported mode {mode}")
    if len(data) != payload:
        raise ValueError(f"{path}: expected {payload} bytes, got {len(data)}")
    return data[9:], w, h, mode


def load_ours_raw(path: str) -> tuple[bytes, int, int]:
    """Ours: 4 w LE, 4 h LE, then RGBA (w*h*4)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8:
        raise ValueError(f"{path}: too short")
    w, h = struct.unpack("<II", data[:8])
    expected = 8 + w * h * 4
    if len(data) != expected:
        raise ValueError(f"{path}: expected {expected} bytes, got {len(data)}")
    return data[8:], w, h


def pixels_match_ref_ours(ref_pixels: bytes, ref_mode: int, ours_pixels: bytes, w: int, h: int) -> tuple[bool, str]:
    """Compare ref raw pixels to ours (RGBA). ref_mode 0=L, 1=RGB."""
    if ref_mode == 0:
        for i in range(w * h):
            r = ours_pixels[i * 4 + 0]
            if ref_pixels[i] != r:
                y, x = divmod(i, w)
                return False, f"first diff at ({x},{y}): ref={ref_pixels[i]} ours_R={r}"
    else:
        for i in range(w * h):
            for c in range(3):
                if ref_pixels[i * 3 + c] != ours_pixels[i * 4 + c]:
                    y, x = divmod(i, w)
                    return False, f"first diff at ({x},{y}) c={c}"
    return True, ""


def main() -> int:
    ap = argparse.ArgumentParser(description="Sanity check libjpeg baseline vs progressive encode/decode")
    ap.add_argument("--work-dir", default="/tmp/jpeg_sanity", help="Directory for PPM, JPEGs, .raw files")
    ap.add_argument("--encode_baseline", default="", help="Path to encode_libjpeg_baseline_scan (default: this dir)")
    ap.add_argument("--dump_jpeg_pixels_ref", default="", help="Path to dump_jpeg_pixels_ref (default: this dir)")
    ap.add_argument("--dump_jpeg_raster", default="", help="Path to dump_jpeg_raster (default: build/.../apps)")
    args = ap.parse_args()

    work = args.work_dir
    os.makedirs(work, exist_ok=True)

    encode_baseline = args.encode_baseline or os.path.join(ORACLE_DIR, "encode_libjpeg_baseline_scan")
    ref_decoder = args.dump_jpeg_pixels_ref or os.path.join(ORACLE_DIR, "dump_jpeg_pixels_ref")
    our_decoder = args.dump_jpeg_raster
    if not our_decoder:
        for base in [os.path.join(SCRIPT_DIR, "..", "..", "build", "linux", "release", "apps"),
                     os.path.join(SCRIPT_DIR, "..", "..", "..", "build", "linux", "release", "apps")]:
            p = os.path.join(os.path.normpath(base), "dump_jpeg_raster")
            if os.path.isfile(p):
                our_decoder = p
                break
        if not our_decoder:
            our_decoder = os.path.join(SCRIPT_DIR, "dump_jpeg_raster")

    for name, path in [("encode_libjpeg_baseline_scan", encode_baseline),
                       ("dump_jpeg_pixels_ref", ref_decoder),
                       ("dump_jpeg_raster", our_decoder)]:
        if not os.path.isfile(path):
            print(f"ERROR: {name} not found at {path}", file=sys.stderr)
            return 1

    ld_path = os.environ.get("LD_LIBRARY_PATH", "")
    app_dir = os.path.dirname(our_decoder)
    if app_dir and ld_path and app_dir not in ld_path.split(os.pathsep):
        ld_path = app_dir + os.pathsep + ld_path
    elif app_dir:
        ld_path = app_dir
    env = os.environ.copy()
    if ld_path:
        env["LD_LIBRARY_PATH"] = ld_path

    # 16×16 grayscale PPM (same pattern as test)
    ppm_path = os.path.join(work, "test_16x16.ppm")
    with open(ppm_path, "wb") as f:
        f.write(f"P5\n{W} {H}\n255\n".encode())
        for y in range(H):
            for x in range(W):
                f.write(bytes([(x + y * 16) & 0xFF]))

    lj_baseline_jpg = os.path.join(work, "libjpeg_baseline_16.jpg")
    lj_progressive_jpg = os.path.join(work, "libjpeg_progressive_16.jpg")

    # --- (1) Libjpeg encoding: baseline and progressive ---
    print("(1) Libjpeg encoding (baseline and progressive)...")
    r = subprocess.run(
        [encode_baseline, str(W), str(H), str(QUALITY), os.path.join(work, "scan_lj.bin"), "0", lj_baseline_jpg],
        cwd=SCRIPT_DIR, env=env, capture_output=True, text=True, timeout=10,
    )
    if r.returncode != 0:
        print(f"  encode_libjpeg_baseline_scan failed: {r.stderr or r.stdout}", file=sys.stderr)
        return 1
    baseline_size = os.path.getsize(lj_baseline_jpg)
    print(f"  Libjpeg baseline:  {lj_baseline_jpg} ({baseline_size} bytes)")

    cjpeg = os.environ.get("CJPEG", "cjpeg")
    r2 = subprocess.run(
        [cjpeg, "-progressive", "-grayscale", "-quality", str(QUALITY),
         "-outfile", lj_progressive_jpg, ppm_path],
        capture_output=True, text=True, timeout=10,
    )
    if r2.returncode != 0:
        print(f"  cjpeg -progressive failed: {r2.stderr or r2.stdout}", file=sys.stderr)
        return 1
    progressive_size = os.path.getsize(lj_progressive_jpg)
    print(f"  Libjpeg progressive: {lj_progressive_jpg} ({progressive_size} bytes)")
    print("  (Encoder comparison vs *our* encoder: run test to get our baseline/progressive files and compare sizes or use compare_baseline_scan_oracle for baseline scan bytes.)")
    print()

    # --- (2) Do the two libjpeg encodings decode to the same raster? ---
    # Use ref without -o (parse HASH from stdout); ref -o can fail on some envs (write raw header).
    print("(2) Do libjpeg baseline and progressive decode to the same raster?")
    def ref_decode_hash(jpg: str) -> tuple[int, int, int, int]:
        r = subprocess.run([ref_decoder, jpg], capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            raise RuntimeError(r.stderr or r.stdout or "ref failed")
        # "HASH <hex16> WIDTH <w> HEIGHT <h> MODE L|RGBA|CMYK"
        line = (r.stdout or "").strip()
        parts = line.split()
        if len(parts) < 8 or parts[0] != "HASH" or parts[2] != "WIDTH" or parts[4] != "HEIGHT":
            raise RuntimeError(f"unexpected ref output: {line}")
        hash_val = int(parts[1], 16)
        w, h = int(parts[3]), int(parts[5])
        mode = 0 if parts[7] == "L" else (1 if parts[7] == "RGBA" else 2)
        return hash_val, w, h, mode
    try:
        hash_baseline, wb, hb, mb = ref_decode_hash(lj_baseline_jpg)
        hash_prog, wp, hp, mp = ref_decode_hash(lj_progressive_jpg)
    except RuntimeError as e:
        print(f"  dump_jpeg_pixels_ref failed: {e}", file=sys.stderr)
        return 1
    if (wb, hb, mb) != (wp, hp, mp):
        print(f"  MISMATCH: dimensions/mode differ: baseline {wb}x{hb} mode={mb}, progressive {wp}x{hp} mode={mp}", file=sys.stderr)
        return 1
    if hash_baseline != hash_prog:
        print(f"  NO: libjpeg baseline hash={hash_baseline:016x} != progressive hash={hash_prog:016x} (different rasters)")
        same_raster = False
    else:
        print(f"  YES: libjpeg decode(baseline) == decode(progressive) ({wb}x{hb}, hash={hash_baseline:016x})")
        same_raster = True
    print()

    # --- (3) Does our decoder match libjpeg decode for each? ---
    # Ref hashes L as w*h bytes; we output RGBA so we hash our R channel (same as L for grayscale).
    print("(3) Does our decoder match libjpeg decode for each file?")
    ours_baseline_raw = os.path.join(work, "ours_baseline.raw")
    ours_progressive_raw = os.path.join(work, "ours_progressive.raw")
    env_no_dump = {k: v for k, v in env.items() if not k.startswith("DUMP_") and k != "GIMG_JPEG_TRACE"}
    def ours_hash_from_raw(raw_path: str, w: int, h: int) -> int:
        ours_px, _w, _h = load_ours_raw(raw_path)
        r_channel = bytes(ours_px[i * 4] for i in range(w * h))
        return fnv1a(r_channel)
    all_ok = True
    # Baseline
    r_b = subprocess.run([our_decoder, "-o", ours_baseline_raw, lj_baseline_jpg], env=env_no_dump, capture_output=True, text=True, timeout=10)
    if r_b.returncode != 0:
        print(f"  Baseline: our decoder failed on libjpeg baseline file: {r_b.stderr or 'Failed to decode'}", file=sys.stderr)
        all_ok = False
    else:
        ours_b, wo, ho = load_ours_raw(ours_baseline_raw)
        if (wo, ho) != (wb, hb):
            print(f"  Baseline: our dimensions {wo}x{ho} != ref {wb}x{hb}", file=sys.stderr)
            all_ok = False
        else:
            our_hash_b = ours_hash_from_raw(ours_baseline_raw, wb, hb)
            if our_hash_b != hash_baseline:
                print(f"  Baseline: our decode hash={our_hash_b:016x} != libjpeg {hash_baseline:016x}", file=sys.stderr)
                all_ok = False
            else:
                print(f"  Baseline: our decode matches libjpeg (hash={our_hash_b:016x})")
    # Progressive
    r_p = subprocess.run([our_decoder, "-o", ours_progressive_raw, lj_progressive_jpg], env=env_no_dump, capture_output=True, text=True, timeout=10)
    if r_p.returncode != 0:
        print(f"  Progressive: our decoder failed on libjpeg progressive file: {r_p.stderr or 'Failed to decode'}", file=sys.stderr)
        print("    (cjpeg -progressive default scan structure may differ from what we support.)", file=sys.stderr)
        all_ok = False
    else:
        ours_p, wo, ho = load_ours_raw(ours_progressive_raw)
        if (wo, ho) != (wp, hp):
            print(f"  Progressive: our dimensions {wo}x{ho} != ref {wp}x{hp}", file=sys.stderr)
            all_ok = False
        else:
            our_hash_p = ours_hash_from_raw(ours_progressive_raw, wp, hp)
            if our_hash_p != hash_prog:
                print(f"  Progressive: our decode hash={our_hash_p:016x} != libjpeg {hash_prog:016x}", file=sys.stderr)
                all_ok = False
            else:
                print(f"  Progressive: our decode matches libjpeg (hash={our_hash_p:016x})")
    print()
    if same_raster and all_ok:
        print("All sanity checks passed.")
    elif not same_raster:
        print("(2) libjpeg baseline vs progressive decode to different rasters (encoder choice).")
    if not all_ok:
        print("(3) our decoder did not match libjpeg for one or both files.")
    return 0 if (same_raster and all_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
