#!/usr/bin/env python3
"""
Verify JPEG files written by encode tests (e.g. to tests/out/jpeg/).
- SOI (0xFF 0xD8), open with PIL, verify/decode (Pillow is the oracle for valid JPEG).
- Optional: EXPECTATIONS by filename (dimensions, SOF type: baseline vs progressive).
- JPEG is lossy; we do not compare decoded pixels to pre-encode source here.
  Decoder correctness is tested separately: oracle decodes JPEG to .raw, our decoder
  decodes the same JPEG, and we compare our output to the oracle .raw.

Usage:
  python3 tests/data/jpeg/verify_jpeg_output.py [DIR]
  DIR defaults to tests/out/jpeg (relative to repo root).

Requires: Pillow (pip install Pillow).

Exit: 0 if all files are valid JPEGs and match expectations, 1 otherwise.
"""
import os
import struct
import sys
from typing import Optional
# Pillow is this script's oracle, so it must be the pinned Pillow and not
# whichever one this machine has. This re-execs the whole script into the image
# before anything imports PIL, so a machine without Pillow lands in the
# container rather than dying on the import - and a machine *with* Pillow still
# answers with the pinned one. See tests/data/oracle_reexec.py.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("pillow")


try:
    from PIL import Image
except ImportError:
    Image = None

SOI = bytes([0xFF, 0xD8])

# Optional: expected features per filename. None = no extra checks.
# width, height: exact size from SOF. sof_baseline: True = SOF0, False = SOF2 (progressive).
EXPECTATIONS: dict[str, dict] = {
    "baseline_grayscale.jpg": {"width": 16, "height": 16, "sof_baseline": True},
    "baseline_rgb.jpg": {"width": 8, "height": 8, "sof_baseline": True},
    "quality_low.jpg": {"width": 8, "height": 8, "sof_baseline": True},
    "chroma_420.jpg": {"width": 32, "height": 32, "sof_baseline": True},
    "chroma_422.jpg": {"width": 32, "height": 32, "sof_baseline": True},
    "chroma_444.jpg": {"width": 32, "height": 32, "sof_baseline": True},
    "restart_interval.jpg": {"width": 32, "height": 32, "sof_baseline": True},
    "progressive_default.jpg": {"width": 16, "height": 16, "sof_baseline": False},
    "progressive_custom.jpg": {"width": 8, "height": 8, "sof_baseline": False},
    "progressive_refinement.jpg": {"width": 16, "height": 16, "sof_baseline": False},
    "baseline_gray16.jpg": {"width": 16, "height": 16},
    "baseline_rgb16.jpg": {"width": 8, "height": 8},
    "progressive_gray16.jpg": {"width": 16, "height": 16, "sof_baseline": False},
    "progressive_rgb16.jpg": {"width": 8, "height": 8, "sof_baseline": False},
    "baseline_gray12.jpg": {"width": 16, "height": 16, "sof_marker": 0xC1},
    "baseline_rgb12.jpg": {"width": 8, "height": 8, "sof_marker": 0xC1},
    "progressive_gray12.jpg": {"width": 16, "height": 16, "sof_baseline": False},
    "progressive_rgb12.jpg": {"width": 8, "height": 8, "sof_baseline": False},
    "large_640x480_baseline_grayscale.jpg": {"width": 640, "height": 480, "sof_baseline": True},
    "large_640x480_baseline_rgb420.jpg": {"width": 640, "height": 480, "sof_baseline": True},
    "large_640x480_progressive_grayscale.jpg": {"width": 640, "height": 480, "sof_baseline": False},
    "large_640x480_progressive_rgb420.jpg": {"width": 640, "height": 480, "sof_baseline": False},
    "large_640x480_restart.jpg": {"width": 640, "height": 480, "sof_baseline": True},
}


def repo_root() -> str:
    """Directory containing .git or derived from this script's path (tests/data/jpeg)."""
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(4):
        if os.path.isdir(os.path.join(d, ".git")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return d


def check_soi(path: str) -> bool:
    """Return True if file starts with SOI (0xFF 0xD8)."""
    try:
        with open(path, "rb") as f:
            return f.read(2) == SOI
    except OSError:
        return False


def read_sof_marker(path: str) -> Optional[int]:
    """
    Scan for first SOF marker (0xC0 baseline, 0xC1 extended, 0xC2 progressive, etc.).
    Returns marker byte (e.g. 0xC0) or None if not found.
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < 4 or data[:2] != SOI:
        return None
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m == 0x00:  # stuffed
            i += 2
            continue
        if 0xC0 <= m <= 0xC3 or 0xC5 <= m <= 0xCF:  # SOF range
            return m
        if m in (0xD8, 0xD9):  # SOI, EOI
            i += 2
            continue
        # Skip this segment
        if i + 4 > len(data):
            break
        length = (data[i + 2] << 8) | data[i + 3]
        if length < 2:
            i += 2
            continue
        i += 2 + length
    return None


def read_sof_dimensions(path: str) -> Optional[tuple[int, int]]:
    """
    Find first SOF segment and return (width, height) from payload.
    SOF payload: 2 bytes length, 1 byte precision, 2 bytes height, 2 bytes width (big-endian).
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < 4 or data[:2] != SOI:
        return None
    i = 2
    while i + 9 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m == 0x00:
            i += 2
            continue
        if 0xC0 <= m <= 0xC3 or 0xC5 <= m <= 0xCF:
            length = (data[i + 2] << 8) | data[i + 3]
            if length >= 8:
                height = (data[i + 5] << 8) | data[i + 6]
                width = (data[i + 7] << 8) | data[i + 8]
                return (width, height)
            return None
        if m in (0xD8, 0xD9):
            i += 2
            continue
        if i + 4 > len(data):
            break
        length = (data[i + 2] << 8) | data[i + 3]
        if length < 2:
            i += 2
            continue
        i += 2 + length
    return None


def read_scan_headers(path: str) -> tuple[bool, list[tuple]]:
    """
    Return (is_progressive, [(Ns, Ss, Se, Ah, Al) for each SOS]).

    Entropy-coded data is skipped by scanning to the next marker that is not a
    stuffed 0x00 or a restart marker (T.81 B.1.1.2, B.1.1.5).
    """
    with open(path, "rb") as fh:
        d = fh.read()
    i = 2
    progressive = False
    out: list[tuple] = []
    while i + 4 <= len(d):
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        seg_len = (d[i + 2] << 8) | d[i + 3]
        if m == 0xC2:
            progressive = True
        if m == 0xDA:
            ns = d[i + 4]
            tail = i + 5 + ns * 2
            if tail + 2 >= len(d):
                break
            out.append((ns, d[tail], d[tail + 1], d[tail + 2] >> 4, d[tail + 2] & 0x0F))
            j = i + 2 + seg_len
            while j + 1 < len(d):
                if d[j] == 0xFF and d[j + 1] != 0 and not (0xD0 <= d[j + 1] <= 0xD7):
                    break
                j += 1
            i = j
            continue
        i += 2 + seg_len
    return progressive, out


def verify_progressive_scan_conformance(path: str, name: str) -> list[str]:
    """
    Check the scan script of a progressive file against T.81 Annex G.1.2.

    - G.1.2.2: "In a scan with Ss not equal to zero, Ns shall be one."  An AC
      scan is always non-interleaved; only a DC scan may carry several
      components.  This encoder used to write every scan with every component,
      producing files libjpeg rejects outright, and nothing noticed because our
      own decoder had the same blind spot.
    - G.1.2: a DC scan has Ss = Se = 0.
    - G.1.1.1.2: successive approximation proceeds one bit at a time, so a
      refinement scan has Ah = Al + 1.
    - Ss <= Se <= 63 (B.2.3).
    """
    errors: list[str] = []
    try:
        progressive, scans = read_scan_headers(path)
    except Exception as exc:  # pragma: no cover - unreadable file
        return [f"{name}: could not read scan headers: {exc}"]
    if not progressive:
        return errors
    for idx, (ns, ss, se, ah, al) in enumerate(scans):
        if se > 63 or ss > se:
            errors.append(f"{name}: scan {idx} has Ss={ss} Se={se} (T.81 B.2.3)")
        if ss == 0 and se != 0:
            errors.append(
                f"{name}: scan {idx} is a DC scan (Ss=0) but Se={se} (T.81 G.1.2)"
            )
        if ss != 0 and ns != 1:
            errors.append(
                f"{name}: scan {idx} has Ss={ss} and Ns={ns}; an AC scan must name "
                f"exactly one component (T.81 G.1.2.2)"
            )
        if ah != 0 and ah != al + 1:
            errors.append(
                f"{name}: scan {idx} has Ah={ah} Al={al}; successive approximation "
                f"refines one bit at a time (T.81 G.1.1.1.2)"
            )
    return errors


def verify_file_features(path: str, name: str, expect: dict) -> list[str]:
    """Verify file matches expected dimensions and SOF type (from SOF segment, no full decode)."""
    errors: list[str] = []
    sof = read_sof_marker(path)
    if sof is None:
        return [f"{name}: could not find SOF marker"]
    dims = read_sof_dimensions(path) if ("width" in expect or "height" in expect) else None
    if "width" in expect or "height" in expect:
        if dims is None:
            errors.append(f"{name}: could not read dimensions from SOF")
        else:
            w, h = dims
            if "width" in expect and w != expect["width"]:
                errors.append(f"{name}: expected width {expect['width']}, got {w}")
            if "height" in expect and h != expect["height"]:
                errors.append(f"{name}: expected height {expect['height']}, got {h}")
    if "sof_baseline" in expect:
        is_baseline = sof == 0xC0
        if expect["sof_baseline"] and not is_baseline:
            errors.append(f"{name}: expected SOF0 (baseline), got SOF 0x{sof:02X}")
        if not expect["sof_baseline"] and is_baseline:
            errors.append(f"{name}: expected SOF2 (progressive), got SOF0")
    if "sof_marker" in expect:
        if sof != expect["sof_marker"]:
            errors.append(
                f"{name}: expected SOF 0x{expect['sof_marker']:02X}, got 0x{sof:02X}"
            )
    return errors


def verify_directory(dirpath: str) -> tuple[int, list[str]]:
    """
    Check all .jpg/.jpeg files in dirpath. Return (failure_count, list of error messages).
    - SOI and PIL Image.open().verify() for every file (Pillow as oracle for valid structure).
    - Full decode (load) is not required; dimensions/SOF for EXPECTATIONS come from SOF parsing.
    """
    errors: list[str] = []
    if not os.path.isdir(dirpath):
        return 1, [f"Not a directory: {dirpath}"]
    if Image is None:
        return 1, [
            "Pillow (PIL) is required for JPEG verification. Install with: pip install Pillow"
        ]
    for name in sorted(os.listdir(dirpath)):
        if not (name.lower().endswith(".jpg") or name.lower().endswith(".jpeg")):
            continue
        path = os.path.join(dirpath, name)
        if not os.path.isfile(path):
            continue
        if not check_soi(path):
            errors.append(f"{name}: invalid JPEG signature (expected SOI 0xFF 0xD8)")
            continue
        pil_ok = False
        try:
            with Image.open(path) as im:
                im.verify()
            pil_ok = True
        except Exception as e:
            # PIL may not support 16-bit or extended-precision JPEG (SOF1, etc.)
            if "cannot identify" in str(e).lower() or "cannot load" in str(e).lower():
                pil_ok = False  # Fall back to SOF/dimension check only
            else:
                errors.append(f"{name}: PIL verify failed: {e}")
                continue
        if not pil_ok:
            # 16-bit or unsupported: still require SOI (done) and valid SOF if in EXPECTATIONS
            if read_sof_marker(path) is None:
                errors.append(f"{name}: PIL could not open and no SOF marker found")
                continue
        # The scan script is checked for every progressive file we write, not
        # only the ones with recorded expectations: T.81 Annex G constrains it
        # regardless of what the file is for, and a violation makes the file
        # unreadable to other decoders.
        errors.extend(verify_progressive_scan_conformance(path, name))
        expect = EXPECTATIONS.get(name)
        if expect is not None:
            errors.extend(verify_file_features(path, name, expect))
    return len(errors), errors


def main() -> int:
    if len(sys.argv) > 1:
        out_dir = os.path.abspath(sys.argv[1])
    else:
        root = repo_root()
        out_dir = os.path.join(root, "tests", "out", "jpeg")
    nerr, errs = verify_directory(out_dir)
    if nerr:
        for msg in errs:
            print(msg, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
