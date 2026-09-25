#!/usr/bin/env python3
"""Check what the BMP encoder wrote, with decoders that are not ours.

The encode tests write each file they produce into tests/out/bmp/ together
with a sidecar `<name>.bmp.expected.rgba` holding the pixels they meant to
write, top-down, four bytes per pixel.  This script decodes each `.bmp` with
whatever outside decoder is installed and compares it against that sidecar.

The point is that our decoder agreeing with our encoder proves nothing about
either: a channel swap, a row flip or a stride error that both halves share
reads as success from the inside.  PNG and JPEG are each checked this way
already (`verify_png_output.py`, `verify_jpeg_output.py`) and BMP was the one
that round-tripped only through itself.

Two decoders are tried, and a file passes when at least one of them reads it
and agrees:

  - **Pillow**, which covers the uncompressed depths this encoder writes.
  - **GdkPixbuf**, which covers RLE4 - where Pillow is wrong, as
    `bmpsuite_sweep.py` documents at more length.

Neither honours a BMP's alpha channel, so alpha is checked only where the
sidecar says the image is opaque; the round-trip tests in
`tests/codec/bmp/test_bmp_encode.cpp` carry the alpha cases, and
`bmpsuite_sweep.py` checks the decoder that reads them back.

Usage:  python3 tests/data/bmp/verify_bmp_output.py [DIR]
        DIR defaults to tests/out/bmp.

Exit: 0 when every file was read by at least one decoder and matched.
"""

import os
import sys
# Pillow is this script's oracle, so it must be the pinned Pillow and not
# whichever one this machine has. This re-execs the whole script into the image
# before anything imports PIL, so a machine without Pillow lands in the
# container rather than dying on the import - and a machine *with* Pillow still
# answers with the pinned one. See tests/data/oracle_reexec.py.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("pillow")



def load_pillow(path):
    from PIL import Image
    with Image.open(path) as im:
        im.load()
        return im.convert("RGBA").tobytes(), im.size


def load_pixbuf(path):
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf
    pb = GdkPixbuf.Pixbuf.new_from_file(path)
    w, h, rowstride = pb.get_width(), pb.get_height(), pb.get_rowstride()
    channels, data = pb.get_n_channels(), pb.get_pixels()
    out = bytearray()
    for y in range(h):
        row = data[y * rowstride:y * rowstride + w * channels]
        if channels == 4:
            out += row
        else:
            for x in range(w):
                out += row[x * 3:x * 3 + 3] + b"\xff"
    return bytes(out), (w, h)


# bmplib is not installed by any package here; it is built from source by
# tools/oracle/fetch.sh.  It is listed because it is the only decoder reachable
# from here that reads what this writer can now produce at a caller's request:
# Pillow refuses a 2-bit BMP outright ("Unsupported BMP pixel depth"), and
# nothing installed reads OS/2 RLE24 at all.  Without it those files would have
# no outside opinion, which this script treats as a failure rather than a pass.
BMPLIB_TOOL = os.environ.get(
    "BMP_ORACLE_BMPLIB",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "..", "tools", "bmp-oracle", "build",
                 "dump_bmp_pixels_bmplib"))


def load_bmplib(path):
    import subprocess
    if not os.path.isfile(BMPLIB_TOOL):
        raise FileNotFoundError(BMPLIB_TOOL)
    proc = subprocess.run([BMPLIB_TOOL, path], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode(errors="replace").strip())
    data = proc.stdout
    if len(data) < 12 or data[:4] != b"BMPO":
        raise RuntimeError("tool wrote no raster")
    w = int.from_bytes(data[4:8], "little")
    h = int.from_bytes(data[8:12], "little")
    return bytes(data[12:]), (w, h)


DECODERS = (("Pillow", load_pillow), ("GdkPixbuf", load_pixbuf),
            ("bmplib", load_bmplib))


def compare(got, want, check_alpha):
    """Return None when they agree, or a sentence saying how they do not."""
    if len(got) != len(want):
        return "decoded %d bytes, expected %d" % (len(got), len(want))
    for i in range(len(got)):
        if i % 4 == 3 and not check_alpha:
            continue
        if got[i] != want[i]:
            pixel = i // 4
            channel = "rgba"[i % 4]
            return ("pixel %d channel %s is %d, expected %d"
                    % (pixel, channel, got[i], want[i]))
    return None


def verify_directory(dirpath):
    errors = []
    checked = 0

    if not os.path.isdir(dirpath):
        return ["not a directory: %s" % dirpath], 0

    names = sorted(n for n in os.listdir(dirpath) if n.lower().endswith(".bmp"))
    if not names:
        # The encode tests had not run, or wrote nothing.  Say so rather than
        # passing: a verifier that silently checks nothing is worse than none.
        return ["no .bmp files in %s; run the encode tests first" % dirpath], 0

    available = [(name, fn) for name, fn in DECODERS if importable(fn)]
    if not available:
        return ["no outside BMP decoder is available (Pillow, GdkPixbuf or "
                "bmplib); install one: pip install Pillow, or run "
                "tools/oracle/fetch.sh bmplib"], 0

    for name in names:
        path = os.path.join(dirpath, name)
        sidecar = path + ".expected.rgba"
        if not os.path.isfile(sidecar):
            errors.append("%s: no %s beside it, so nothing says what it should "
                          "contain" % (name, os.path.basename(sidecar)))
            continue
        with open(sidecar, "rb") as f:
            want = f.read()
        # An all-opaque expectation is one whose alpha the decoders can be
        # asked about; anything else, they would answer 255 regardless.
        check_alpha = all(want[i] == 255 for i in range(3, len(want), 4))

        agreed = False
        for decoder_name, fn in available:
            try:
                got, _ = fn(path)
            except Exception:
                continue  # a decoder that cannot read it is not evidence
            problem = compare(got, want, check_alpha)
            if problem:
                errors.append("%s: %s reads it as %s"
                              % (name, decoder_name, problem))
            else:
                agreed = True
        if agreed:
            checked += 1
        else:
            # Saying only "nothing read it" is true and unhelpful when the
            # one decoder that could is a source build away.  A 2-bit or an
            # OS/2 RLE24 file is exactly that case: Pillow refuses the first
            # outright and nothing installed reads the second, which is also
            # why the writer only produces either when asked to.
            hint = ""
            if not any(n == "bmplib" for n, _ in available):
                hint = (" - bmplib is not built, and it is the only decoder "
                        "here that reads 2-bit or OS/2 RLE24 files; run "
                        "tools/oracle/fetch.sh bmplib")
            errors.append(
                "%s: no outside decoder read it and agreed%s" % (name, hint))

    return errors, checked


def importable(fn):
    try:
        fn(os.devnull)
    except (ImportError, FileNotFoundError):
        # Not installed, or - for bmplib - not built.  Either way it has no
        # opinion to offer, which is different from disagreeing.
        return False
    except Exception:
        return True
    return True


def main():
    if len(sys.argv) > 1:
        out_dir = os.path.abspath(sys.argv[1])
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(os.path.dirname(here))
        out_dir = os.path.join(root, "tests", "out", "bmp")

    errors, checked = verify_directory(out_dir)
    if errors:
        for message in errors:
            print(message, file=sys.stderr)
        return 1
    print("  %d encoder outputs read back and matched" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
