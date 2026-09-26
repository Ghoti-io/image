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
"""Decode many JPEGs with the pinned Pillow in one process.

This is the batch half of a comparison whose other half is our own host-built
decoder, so the caller cannot re-exec itself into the image the way the
verify_* scripts do. Asking per file instead would be one container start per
file, and the corpus generate_matrix.py writes is about twelve hundred of
them.

Protocol, stdin to stdout, so that nothing about it depends on the caller and
the image sharing a working directory:

  in   one absolute path per line
  out  one line per file, TAB-separated:
         <index>  ok    <width> <height> <rgb-file>
         <index>  fail  <reason>
       and last, alone on a line, the number of inputs answered

The RGB is written as raw bytes to <scratch>/<index>.rgb rather than sent down
the pipe, so the caller reads what it needs and nothing has to be framed.

  decode_batch_pillow.py <scratch-directory>

The count on the last line is the denominator the caller checks its own
against: an oracle that wrote nothing and exited 0 would otherwise read as
agreement on every file.
"""

import os
import sys

from PIL import Image


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: decode_batch_pillow.py <scratch-directory>\n")
        return 2
    scratch = os.path.abspath(sys.argv[1])
    answered = 0
    for index, line in enumerate(sys.stdin):
        path = line.strip()
        if not path:
            continue
        try:
            im = Image.open(path)
            im.load()
            rgb = im.convert("RGB")
            out = os.path.join(scratch, "%d.rgb" % index)
            with open(out, "wb") as f:
                f.write(rgb.tobytes())
            sys.stdout.write("%d\tok\t%d\t%d\t%s\n"
                             % (index, rgb.width, rgb.height, out))
        except Exception as exc:                      # noqa: BLE001
            # A file this reference refuses is an answer, not a failure of the
            # batch: the corpus is a cross-product and some of it is meant to
            # be refused. The caller decides what to make of it.
            sys.stdout.write("%d\tfail\t%s\n" % (index, str(exc)[:120]))
        answered += 1
    sys.stdout.write("%d\n" % answered)
    return 0


if __name__ == "__main__":
    sys.exit(main())
