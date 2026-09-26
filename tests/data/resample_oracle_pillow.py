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
"""Pillow's answer to a batch of resampling questions.

verify_resample.py is the one verification script that cannot simply re-exec
itself into the pinned image, because it also runs *this library's*
resample_tool, and a host-built binary is not something the oracle image should
be asked to run. So the reference half is split out here and asked once, as a
batch, which is the protocol every oracle in this suite already speaks:
one process per batch, one case per line, nothing forked per case.

Input, one case per line on stdin, tab-separated:

    <in_path> <mode> <src_w> <src_h> <dst_w> <dst_h> <pillow_filter> <out_path>

`in_path` holds the raw samples the resampler was given - the same bytes, read
rather than regenerated, so that the reference answers for exactly what our
tool was asked and not for a second copy of the generator that could drift from
the first.

Output: the resized bytes at each `out_path`, and one line on stdout saying how
many were written, which is the caller's denominator.
"""

import sys

from PIL import Image


def main():
    written = 0
    for line in sys.stdin:
        line = line.rstrip("\n")
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) != 8:
            sys.stderr.write("resample oracle: not eight fields: %r\n" % line)
            return 2
        in_path, mode, sw, sh, dw, dh, filt, out_path = parts
        with open(in_path, "rb") as handle:
            raw = handle.read()
        img = Image.frombytes(mode, (int(sw), int(sh)), raw)
        resampling = getattr(Image.Resampling, filt)
        with open(out_path, "wb") as handle:
            handle.write(img.resize((int(dw), int(dh)), resampling).tobytes())
        written += 1
    sys.stdout.write("%d\n" % written)
    return 0


if __name__ == "__main__":
    sys.exit(main())
