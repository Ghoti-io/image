#!/usr/bin/env python3
"""
Verify PNG files written by encode tests (e.g. to tests/out/png/).
Checks PNG signature, IHDR presence, and full decode via PIL (required).

Usage:
  python3 tests/data/png/verify_png_output.py [DIR]
  DIR defaults to tests/out/png (relative to repo root).

Requires: Pillow (pip install Pillow).

Exit: 0 if all files are valid PNGs, 1 otherwise.
"""
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    Image = None

# PNG signature (8 bytes)
PNG_SIG = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])


def repo_root() -> str:
    """Directory containing .git or this script's tests/data/png."""
    d = os.path.dirname(os.path.abspath(__file__))
    # We're in tests/data/png; repo root is 3 levels up
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


def check_png_ihdr(path: str) -> bool:
    """Return True if file has valid signature and IHDR chunk (length 13)."""
    try:
        with open(path, "rb") as f:
            sig = f.read(8)
            if sig != PNG_SIG:
                return False
            # First chunk must be IHDR: 4-byte length (13), 4-byte type "IHDR"
            chunk_len = f.read(4)
            chunk_type = f.read(4)
            if len(chunk_len) != 4 or len(chunk_type) != 4:
                return False
            length = struct.unpack(">I", chunk_len)[0]
            if chunk_type != b"IHDR" or length != 13:
                return False
            return True
    except OSError:
        return False


def verify_directory(dirpath: str) -> tuple[int, list[str]]:
    """
    Check all .png files in dirpath. Return (failure_count, list of error messages).
    Always runs PIL Image.verify() on each PNG (Pillow required).
    """
    errors = []
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
