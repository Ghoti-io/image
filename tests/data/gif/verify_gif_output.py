#!/usr/bin/env python3
"""Check what the GIF encoder wrote, with decoders that are not ours.

The encode tests write each file they produce into tests/out/gif/ together with
one sidecar per frame, `<name>.gif.expected.<i>.rgba`, holding the pixels that
frame was meant to show once composited onto the logical screen - top to
bottom, four bytes per pixel.  This script decodes each `.gif` with whatever
outside decoder is installed and compares.

Our decoder agreeing with our encoder proves nothing about either: a channel
swap, a row order mistake or a palette off-by-one that both halves share reads
as success from the inside.  PNG, JPEG and BMP are each checked this way
already.

Four decoders are tried, and each is asked only about what it is good for:

  - **giflib**, through tools/oracle's dump_gif_pixels_giflib.  It is the
    reference implementation and is authoritative for LZW, interlace and the
    colour tables.  It does not composite, so that tool does it from the
    specification - which makes giflib independent evidence about
    decompression and *not* about disposal, because the compositing is ours.
  - **ImageMagick**, through `magick -coalesce`, which has its own GIF reader
    rather than linking giflib.  It is the independent opinion about
    compositing, and the reason it is here is below.
  - **Pillow**, asked about the first frame only.
  - **GdkPixbuf**, which reads the first frame.

WHY PILLOW IS ASKED ABOUT ONE FRAME
===================================

Pillow does not agree about what "restore to background" (disposal 2, 89a 23)
leaves behind.  For a two-frame file whose second frame is transparent where
the first was opaque, this codec and ImageMagick both give a transparent
pixel; Pillow gives an opaque one - sometimes the background colour, sometimes
the previous frame showing through.  Adding a Global Color Table, which was
the obvious suspect, changes nothing either way.

Two implementations against one is not by itself an argument, so note which
two: ImageMagick's reader is independent of giflib and of this library, and it
matches what 89a 23 says the area is restored to.  Pillow is therefore asked
about frame 0, where it checks the palette, the LZW and the transparent index
without any disposal being involved, and is not asked about later frames.

WHAT IS NOT COMPARED
====================

The colour underneath a fully transparent pixel.  GIF stores an index, and the
index that a Graphic Control Extension names transparent still points at a
colour table entry; what a decoder puts in the red, green and blue channels of
a pixel it has just declared invisible is its own business.  Pillow writes the
palette colour, giflib and this library write zero.  Measured across 121 GIFs
installed on one Debian machine, that accounts for every difference between
this decoder and Pillow: 128 frames differed and not one of them differed in a
pixel either side called visible.  Comparing those channels would fail on a
disagreement that cannot be seen.

Usage:  python3 tests/data/gif/verify_gif_output.py [DIR]
        DIR defaults to tests/out/gif.

Exit: 0 when every frame was read by at least one decoder and matched.
"""

import os
import struct
import subprocess
import sys
# Pillow is this script's oracle, so it must be the pinned Pillow and not
# whichever one this machine has. This re-execs the whole script into the image
# before anything imports PIL, so a machine without Pillow lands in the
# container rather than dying on the import - and a machine *with* Pillow still
# answers with the pinned one. See tests/data/oracle_reexec.py.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("pillow")


GIFLIB_TOOL = os.environ.get(
    "GIF_ORACLE_GIFLIB",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "..", "tools", "gif-oracle", "build",
                 "dump_gif_pixels_giflib"))


def load_giflib(path, frame):
    if not os.path.isfile(GIFLIB_TOOL):
        raise FileNotFoundError(GIFLIB_TOOL)
    proc = subprocess.run(
        [GIFLIB_TOOL, "--composite", "--frame", str(frame), path],
        capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode(errors="replace").strip())
    data = proc.stdout
    if len(data) < 12 or data[:4] != b"GIFO":
        raise RuntimeError("tool wrote no raster")
    w, h = struct.unpack("<II", data[4:12])
    return bytes(data[12:12 + w * h * 4]), (w, h)


def load_pillow(path, frame):
    from PIL import Image
    with Image.open(path) as im:
        im.seek(frame)
        rgba = im.convert("RGBA")
        return rgba.tobytes(), rgba.size


def load_imagemagick(path, frame):
    """All frames, disposal applied, as raw RGBA."""
    proc = subprocess.run(
        ["magick", path, "-coalesce", "-depth", "8", "RGBA:-"],
        capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode(errors="replace").strip())
    data = proc.stdout
    # It writes the frames back to back and says nothing about the size, so
    # the caller's expectation is what says where one ends.
    return data, frame


def load_pixbuf(path, frame):
    if frame != 0:
        # GdkPixbuf's simple loader gives the first frame only; say so rather
        # than silently comparing the wrong one.
        raise RuntimeError("reads the first frame only")
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


# (name, loader, frames it is asked about): "first" means frame 0 only.
DECODERS = (("giflib", load_giflib, "all"),
            ("ImageMagick", load_imagemagick, "all"),
            ("Pillow", load_pillow, "first"),
            ("GdkPixbuf", load_pixbuf, "first"))


def importable(fn, path, frame):
    try:
        fn(path, frame)
        return True
    except (ImportError, ValueError, FileNotFoundError):
        return False
    except Exception:
        # It is installed; it just did not like this file or this frame.
        return True


def compare(got, want):
    """Return None when they agree, or a sentence saying how they do not."""
    if len(got) != len(want):
        return "decoded %d bytes, expected %d" % (len(got), len(want))
    for off in range(0, len(got), 4):
        a = got[off:off + 4]
        b = want[off:off + 4]
        if a == b:
            continue
        if a[3] == 0 and b[3] == 0:
            continue  # both invisible; see the module docstring
        pixel = off // 4
        return ("pixel %d is rgba(%d,%d,%d,%d), expected rgba(%d,%d,%d,%d)"
                % (pixel, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]))
    return None


def frames_for(dirpath, name):
    """The frame indices that have a sidecar, in order."""
    prefix = name + ".expected."
    out = []
    for entry in os.listdir(dirpath):
        if entry.startswith(prefix) and entry.endswith(".rgba"):
            middle = entry[len(prefix):-len(".rgba")]
            if middle.isdigit():
                out.append(int(middle))
    return sorted(out)


def verify_directory(dirpath):
    errors = []
    checked = 0

    if not os.path.isdir(dirpath):
        return ["not a directory: %s" % dirpath], 0

    names = sorted(n for n in os.listdir(dirpath) if n.lower().endswith(".gif"))
    if not names:
        # The encode tests had not run, or wrote nothing.  Say so rather than
        # passing: a verifier that silently checks nothing is worse than none.
        return ["no .gif files in %s; run the encode tests first" % dirpath], 0

    for name in names:
        path = os.path.join(dirpath, name)
        problem = check_comment(dirpath, name, path)
        if problem:
            errors.append(problem)
        indices = frames_for(dirpath, name)
        if not indices:
            errors.append("%s: no %s.expected.<i>.rgba beside it, so nothing "
                          "says what it should contain" % (name, name))
            continue

        for frame in indices:
            sidecar = os.path.join(dirpath, "%s.expected.%d.rgba" % (name, frame))
            with open(sidecar, "rb") as f:
                want = f.read()

            agreed = False
            read_by_someone = False
            for decoder_name, fn, scope in DECODERS:
                if scope == "first" and frame != 0:
                    continue
                try:
                    got, _ = fn(path, frame)
                except Exception:
                    continue  # a decoder that cannot read it is not evidence
                if decoder_name == "ImageMagick":
                    # It returned every frame; take the one asked for.
                    start = frame * len(want)
                    if len(got) < start + len(want):
                        continue
                    got = got[start:start + len(want)]
                read_by_someone = True
                problem = compare(got, want)
                if problem:
                    errors.append("%s frame %d: %s reads it as %s"
                                  % (name, frame, decoder_name, problem))
                else:
                    agreed = True
            if not read_by_someone:
                errors.append(
                    "%s frame %d: no outside decoder read it. Install Pillow "
                    "(pip install Pillow) or ImageMagick, or build the giflib "
                    "oracle (make oracle-build oracle-tools, which needs libgif-dev)."
                    % (name, frame))
            elif agreed:
                checked += 1

    return errors, checked


def comment_pillow(path):
    from PIL import Image
    with Image.open(path) as im:
        raw = im.info.get("comment")
    return raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw


def comment_imagemagick(path):
    proc = subprocess.run(["magick", "identify", "-format", "%c", path],
                          capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode(errors="replace").strip())
    return proc.stdout.decode("utf-8", "replace")


def check_comment(dirpath, name, path):
    """Ask outside decoders to read back the comment the encoder wrote.

    Containment rather than equality, because the two decoders disagree about
    a file holding several comments and neither is wrong: reading one GIF with
    two, ImageMagick reports the last and Pillow reports both joined by a
    newline. What is being checked is that the text this library wrote is
    there and readable, not whose joining rule is right.
    """
    sidecar = os.path.join(dirpath, name + ".expected.comment")
    if not os.path.exists(sidecar):
        return None
    with open(sidecar, "rb") as f:
        want = f.read().decode("utf-8")

    read_by_someone = False
    for decoder_name, fn in (("Pillow", comment_pillow),
                             ("ImageMagick", comment_imagemagick)):
        try:
            got = fn(path)
        except Exception:
            continue  # a decoder that is absent or unhappy is not evidence
        if got is None:
            return ("%s: %s read the file but found no comment, and one was "
                    "written" % (name, decoder_name))
        read_by_someone = True
        if want not in got:
            return ("%s: %s reads the comment as %r, which does not contain "
                    "the %r that was written" % (name, decoder_name, got, want))
    if not read_by_someone:
        return ("%s: a comment was written and no outside decoder read it. "
                "Install Pillow (pip install Pillow) or ImageMagick." % name)
    return None


def main():
    dirpath = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "gif")
    errors, checked = verify_directory(dirpath)
    for e in errors:
        print("  %s" % e, file=sys.stderr)
    if errors:
        print("GIF output verification FAILED", file=sys.stderr)
        return 1
    print("  %d encoder frames read back and matched" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
