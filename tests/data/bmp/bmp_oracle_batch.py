#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
#
# Ghoti.io Image is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""What the four installed BMP decoders make of a batch of files.

bmpsuite_sweep.py runs this library's own dump_bmp_raster, so it cannot
re-exec itself into the pinned image the way the verification scripts do. Its
oracle half is asked here instead, as one process for the whole corpus, which
is the protocol the rest of this suite's oracles already speak.

The four are Pillow, GdkPixbuf, netpbm's bmptopnm and bmplib. All four are
pinned in tools/oracle/containers/IMAGES and live in the same image; before
this they were whatever the machine had installed, and three of them were not
recorded anywhere at all.

Input, one case per line on stdin, tab-separated:

    <oracle> <bmp_path> <out_path>

Output: packed RGBA8 at each `out_path` for every case that decoded, a
`<out_path>.err` holding the reason for every case that did not, and a summary
line per oracle on stdout - `<oracle> <decoded> <refused>` - which is the
caller's denominator. An oracle that reads nothing is not the same as an
oracle that agrees with everything, and only the count separates them.
"""

import os
import subprocess
import sys


def oracle_pil(path):
    from PIL import Image
    with Image.open(path) as im:
        im.load()
        return im.convert("RGBA").tobytes()


def oracle_pixbuf(path):
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
    return bytes(out)


def oracle_netpbm(path):
    proc = subprocess.run(["bmptopnm", path], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode(errors="replace").strip())
    return pnm_to_rgba(proc.stdout)


def pnm_to_rgba(data):
    """Convert a P1/P2/P3/P4/P5/P6 image to packed RGBA8."""
    fields, i = [], 0
    want = 3 if data[:2] in (b"P1", b"P4") else 4
    while len(fields) < want:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    magic, w, h = fields[0], int(fields[1]), int(fields[2])
    maxval = int(fields[3]) if want == 4 else 1
    body = data[i:]
    out = bytearray()
    if magic == b"P6":
        for k in range(w * h):
            out += body[k * 3:k * 3 + 3] + b"\xff"
    elif magic == b"P5":
        for k in range(w * h):
            out += body[k:k + 1] * 3 + b"\xff"
    elif magic == b"P4":
        # Packed bits, one row at a time; 1 is black in PBM.
        stride = (w + 7) // 8
        for y in range(h):
            for x in range(w):
                bit = (body[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1
                v = 0 if bit else 255
                out += bytes((v, v, v, 255))
    else:
        raise RuntimeError("unhandled PNM type " + magic.decode())
    if maxval not in (1, 255) and magic in (b"P5", b"P6"):
        raise RuntimeError("unhandled PNM maxval %d" % maxval)
    return bytes(out)


# bmplib is built into the image from the commit tools/oracle/VERSIONS pins,
# and the dumper that drives it is compiled against that build by
# `make oracle-tools`. It is the only decoder here that reads OS/2 Huffman 1D,
# OS/2 bitmap arrays and 64-bit BMPs.
BMPLIB_TOOL = os.environ.get(
    "BMP_ORACLE_BMPLIB",
    os.path.join(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))),
        "tools", "bmp-oracle", "build", "dump_bmp_pixels_bmplib"))


def oracle_bmplib(path):
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
    body = data[12:]
    if len(body) != w * h * 4:
        raise RuntimeError("tool wrote %d bytes for %dx%d" % (len(body), w, h))
    return bytes(body)


ORACLES = {"pil": oracle_pil, "pixbuf": oracle_pixbuf,
           "netpbm": oracle_netpbm, "bmplib": oracle_bmplib}


def main():
    decoded = {name: 0 for name in ORACLES}
    refused = {name: 0 for name in ORACLES}
    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            sys.stderr.write("bmp oracle: not three fields: %r\n" % line)
            return 2
        oracle, path, out_path = parts
        if oracle not in ORACLES:
            sys.stderr.write("bmp oracle: no such decoder: %s\n" % oracle)
            return 2
        try:
            data = ORACLES[oracle](path)
        except Exception as error:  # a decoder that cannot read a file
            with open(out_path + ".err", "w", encoding="utf-8") as handle:
                handle.write("%s: %s\n" % (type(error).__name__, error))
            refused[oracle] += 1
            continue
        with open(out_path, "wb") as handle:
            handle.write(data)
        decoded[oracle] += 1
    for name in sorted(ORACLES):
        sys.stdout.write("%s\t%d\t%d\n" % (name, decoded[name], refused[name]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
