#!/usr/bin/env python3
"""
Verify PNG files written by encode tests (e.g. to tests/out/png/).
- PNG signature, IHDR presence, and full decode via PIL (required).
- Optional: exact features (IHDR interlace, chunk set) per expected manifest.

Usage:
  python3 tests/data/png/verify_png_output.py [DIR]
  DIR defaults to tests/out/png (relative to repo root).

Requires: Pillow (pip install Pillow).

Exit: 0 if all files are valid PNGs and match expectations, 1 otherwise.
"""
import os
import struct
import sys
from typing import Optional

try:
    from PIL import Image
except ImportError:
    Image = None

# PNG signature (8 bytes)
PNG_SIG = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])

# Optional: expected features per filename. None = no extra checks.
# interlace: 0 or 1 (IHDR byte 13). chunks_include: chunk types that must appear. chunks_exclude: must not appear.
EXPECTATIONS: dict[str, dict] = {
    "interlaced_roundtrip.png": {"interlace": 1},
    "preserve_exif.png": {"chunks_include": [b"eXIf"]},
    "roundtrip_1x1_gray.png": {},
    "roundtrip_1x1_rgba.png": {},
}


def repo_root() -> str:
    """Directory containing .git or this script's tests/data/png."""
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(3):
        if os.path.isdir(os.path.join(d, ".git")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return d


def check_png_signature(path: str) -> bool:
    """Return True if file starts with PNG signature."""
    try:
        with open(path, "rb") as f:
            return f.read(8) == PNG_SIG
    except OSError:
        return False


def read_png_chunks(path: str):
    """
    Read file; return (success, list of (chunk_type, payload_length), IHDR_payload or None).
    IHDR payload is 13 bytes (width, height, bit_depth, color_type, compression, filter, interlace).
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return False, [], None
    if len(data) < 8 + 4 + 4:
        return False, [], None
    if data[:8] != PNG_SIG:
        return False, [], None
    pos = 8
    chunks = []
    ihdr_payload = None  # type: Optional[bytes]
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        ctype = data[pos + 4 : pos + 8]
        pos += 8
        if pos + length + 4 > len(data):
            return False, [], None
        if ctype == b"IHDR" and length == 13:
            ihdr_payload = data[pos : pos + 13]
        chunks.append((ctype, length))
        pos += length + 4
    return True, chunks, ihdr_payload


def check_png_ihdr(path: str) -> bool:
    """Return True if file has valid signature and IHDR chunk (length 13)."""
    ok, chunks, _ = read_png_chunks(path)
    if not ok or not chunks or chunks[0][0] != b"IHDR" or chunks[0][1] != 13:
        return False
    return True


def verify_file_features(path: str, name: str, expect: dict) -> list[str]:
    """Verify file matches expected interlace and chunk set. Return list of error strings."""
    errors: list[str] = []
    ok, chunks, ihdr = read_png_chunks(path)
    if not ok or ihdr is None:
        return [f"{name}: could not parse chunks"]
    chunk_types = [c[0] for c in chunks]
    if "interlace" in expect:
        interlace = ihdr[12]
        if interlace != expect["interlace"]:
            errors.append(
                f"{name}: expected IHDR interlace_method={expect['interlace']}, got {interlace}"
            )
    for req in expect.get("chunks_include", []):
        if req not in chunk_types:
            errors.append(f"{name}: expected chunk {req!r} not found")
    for forbidden in expect.get("chunks_exclude", []):
        if forbidden in chunk_types:
            errors.append(f"{name}: chunk {forbidden!r} should be absent")
    return errors


def verify_directory(dirpath: str) -> tuple[int, list[str]]:
    """
    Check all .png files in dirpath. Return (failure_count, list of error messages).
    - Signature, IHDR, PIL Image.open().verify() for every file.
    - If file is in EXPECTATIONS, also check interlace and chunk set.
    """
    errors: list[str] = []
    if not os.path.isdir(dirpath):
        return 1, [f"Not a directory: {dirpath}"]
    if Image is None:
        return 1, ["Pillow (PIL) is required for PNG verification. Install with: pip install Pillow"]
    for name in sorted(os.listdir(dirpath)):
        if not name.lower().endswith(".png"):
            continue
        path = os.path.join(dirpath, name)
        if not os.path.isfile(path):
            continue
        if not check_png_signature(path):
            errors.append(f"{name}: invalid PNG signature")
            continue
        if not check_png_ihdr(path):
            errors.append(f"{name}: missing or invalid IHDR chunk")
            continue
        try:
            with Image.open(path) as im:
                im.verify()
        except Exception as e:
            errors.append(f"{name}: PIL verify failed: {e}")
            continue
        expect = EXPECTATIONS.get(name)
        if expect is not None:
            errors.extend(verify_file_features(path, name, expect))
    return len(errors), errors


def main() -> int:
    if len(sys.argv) > 1:
        out_dir = os.path.abspath(sys.argv[1])
    else:
        root = repo_root()
        out_dir = os.path.join(root, "tests", "out", "png")
    nerr, errs = verify_directory(out_dir)
    if nerr:
        for msg in errs:
            print(msg, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
