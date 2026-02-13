#!/usr/bin/env python3
"""
Compare our decoder's Y/Cb/Cr component buffers to Pillow-implied Y/Cb/Cr.

Dumps our components by running the decoder with DUMP_JPEG_COMPONENTS=<dir>,
then converts Pillow's RGB to Y/Cb/Cr (inverse BT.601), downsamples Pillow's
chroma to match our 4:2:0 dimensions, and reports per-component differences.

Usage:
  python3 compare_ycbcr_components.py <path-to.jpeg> [dump-dir] [path-to-dump_jpeg_raster]

  If dump-dir is omitted, a temporary directory is used.
  Requires: Pillow, and dump_jpeg_raster built. Set LD_LIBRARY_PATH if needed.

Example (from repo root):
  LD_LIBRARY_PATH=build/linux/release/apps:../compress/build/linux/release/apps \\
  python3 tests/data/jpeg/compare_ycbcr_components.py tests/data/jpeg/baseline_16x16_ycbcr.jpg
"""
import os
import struct
import subprocess
import sys
import tempfile

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required. Install with: pip install Pillow")


def read_component_raw(path: str) -> tuple[bytes, int, int]:
    """Read a .raw file: 4 bytes LE width, 4 bytes LE height, then w*h bytes. Return (data, w, h)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8:
        raise ValueError(f"{path}: too short")
    w, h = struct.unpack("<II", data[:8])
    expected = 8 + w * h
    if len(data) != expected:
        raise ValueError(f"{path}: expected {expected} bytes, got {len(data)}")
    return data[8:], w, h


def rgb_to_ycbcr_bt601(r: int, g: int, b: int) -> tuple[int, int, int]:
    """Inverse of our forward BT.601: Y = 0.299*R + 0.587*G + 0.114*B; Cb/Cr from (B-Y)/(R-Y)."""
    y = 0.299 * r + 0.587 * g + 0.114 * b
    y_int = max(0, min(255, round(y)))
    # Cb = 128 + (B - Y) / 1.772, Cr = 128 + (R - Y) / 1.402 (inverse of our forward)
    cb = 128 + (b - y) / 1.772
    cr = 128 + (r - y) / 1.402
    cb_int = max(0, min(255, round(cb)))
    cr_int = max(0, min(255, round(cr)))
    return y_int, cb_int, cr_int


def downsample_420(data: bytes, w: int, h: int) -> tuple[bytes, int, int]:
    """Downsample by 2x in each dimension (average 2x2 blocks). Returns (bytes, w//2, h//2)."""
    if w % 2 != 0 or h % 2 != 0:
        raise ValueError("dimensions must be even")
    out_w, out_h = w // 2, h // 2
    out = bytearray(out_w * out_h)
    for j in range(out_h):
        for i in range(out_w):
            s = (
                data[(2 * j) * w + 2 * i]
                + data[(2 * j) * w + 2 * i + 1]
                + data[(2 * j + 1) * w + 2 * i]
                + data[(2 * j + 1) * w + 2 * i + 1]
            )
            out[j * out_w + i] = (s + 2) // 4
    return bytes(out), out_w, out_h


def compare_component(name: str, our: bytes, pill: bytes, n: int) -> None:
    """Report difference stats for one component (same length byte arrays)."""
    if len(our) != n or len(pill) != n:
        print(f"  {name}: length mismatch (our={len(our)} pill={len(pill)} n={n})")
        return
    diff_count = sum(1 for a, b in zip(our, pill) if a != b)
    max_d = 0
    sum_d = 0
    for a, b in zip(our, pill):
        d = abs(int(a) - int(b))
        max_d = max(max_d, d)
        sum_d += d
    mean_d = sum_d / n if n else 0
    print(f"  {name}: samples {n}, differ {diff_count} ({100 * diff_count / n:.1f}%), max_abs_diff={max_d}, mean_abs_diff={mean_d:.2f}")


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "Usage: python3 compare_ycbcr_components.py <path-to.jpeg> [dump-dir] [path-to-dump_jpeg_raster]",
            file=sys.stderr,
        )
        return 1
    jpeg_path = sys.argv[1]
    dump_dir = None
    dump_exe = "dump_jpeg_raster"
    if len(sys.argv) == 3:
        # Second arg: dump dir or path to dump_jpeg_raster
        if os.path.isfile(sys.argv[2]):
            dump_exe = sys.argv[2]
        else:
            dump_dir = sys.argv[2] or None
    elif len(sys.argv) >= 4:
        dump_dir = sys.argv[2] if sys.argv[2] else None
        dump_exe = sys.argv[3]

    ld_path = os.environ.get("LD_LIBRARY_PATH", "")
    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 1

    use_tmp = dump_dir is None
    if use_tmp:
        dump_dir = tempfile.mkdtemp(prefix="jpeg_components_")
    else:
        os.makedirs(dump_dir, exist_ok=True)

    env = os.environ.copy()
    env["DUMP_JPEG_COMPONENTS"] = dump_dir
    if ld_path:
        env["LD_LIBRARY_PATH"] = ld_path
    # Forward progressive bisect env so decoder can limit scans.
    for key in ("GIMG_JPEG_PROGRESSIVE_MAX_SCANS",):
        if key in os.environ:
            env[key] = os.environ[key]

    r = subprocess.run(
        [dump_exe, os.path.abspath(jpeg_path)],
        capture_output=True,
        env=env,
        timeout=10,
    )
    if r.returncode != 0:
        print(f"dump_jpeg_raster failed: {r.stderr.decode() or r.returncode}", file=sys.stderr)
        if use_tmp:
            os.rmdir(dump_dir)
        return 1

    for name in ("Y.raw", "Cb.raw", "Cr.raw"):
        path = os.path.join(dump_dir, name)
        if not os.path.isfile(path):
            print(f"Our decoder did not write {path} (not baseline 3-component?).", file=sys.stderr)
            if use_tmp:
                import shutil
                shutil.rmtree(dump_dir, ignore_errors=True)
            return 1

    our_y, w_y, h_y = read_component_raw(os.path.join(dump_dir, "Y.raw"))
    our_cb, w_cb, h_cb = read_component_raw(os.path.join(dump_dir, "Cb.raw"))
    our_cr, w_cr, h_cr = read_component_raw(os.path.join(dump_dir, "Cr.raw"))

    with Image.open(jpeg_path) as im:
        im.load()
    if im.mode != "RGB":
        im = im.convert("RGB")
    rgb = im.tobytes()
    w, h = im.size
    if w != w_y or h != h_y:
        print(f"Dimension mismatch: Pillow {w}x{h}, our Y {w_y}x{h_y}", file=sys.stderr)
        if use_tmp:
            import shutil
            shutil.rmtree(dump_dir, ignore_errors=True)
        return 1

    n_pix = w * h
    pill_y = bytearray(n_pix)
    pill_cb_full = bytearray(n_pix)
    pill_cr_full = bytearray(n_pix)
    for i in range(n_pix):
        r = rgb[i * 3]
        g = rgb[i * 3 + 1]
        b = rgb[i * 3 + 2]
        y, cb, cr = rgb_to_ycbcr_bt601(r, g, b)
        pill_y[i] = y
        pill_cb_full[i] = cb
        pill_cr_full[i] = cr

    pill_cb, pw_cb, ph_cb = downsample_420(bytes(pill_cb_full), w, h)
    pill_cr, pw_cr, ph_cr = downsample_420(bytes(pill_cr_full), w, h)

    print("Component comparison (our decode Y/Cb/Cr vs Pillow RGB → inverse BT.601 Y/Cb/Cr)")
    print(f"Image: {jpeg_path}, size {w} x {h}")
    print("")
    compare_component("Y (luma)", our_y, bytes(pill_y), n_pix)
    compare_component("Cb (4:2:0)", our_cb, pill_cb, w_cb * h_cb)
    compare_component("Cr (4:2:0)", our_cr, pill_cr, w_cr * h_cr)
    print("")
    print("If Y/Cb/Cr match closely, the remaining RGB difference is from YCbCr→RGB rounding.")
    print("If Y/Cb/Cr differ, the gap is in IDCT, dequantization, or level shift.")

    if use_tmp:
        import shutil
        shutil.rmtree(dump_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
