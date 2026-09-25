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
"""Put a whole verification script inside the pinned oracle image.

The scripts that import it - verify_png_output.py and its four siblings - read
what this library wrote and decode it with Pillow. Pillow *is* the oracle
there, so "whichever Pillow this machine has" is the same defect the container
work exists to close, and it is a quieter one than the gtest skips: these
scripts do not skip, they simply measure a different reference and report
agreement with it.

They are converted by re-exec rather than by a batch protocol, which is what
`regex` did to its in-process python gate (notes/suite/CONTAINERS.md section
2.4). The difference is what each script is: regex's tool asked `re` one
question per case and the process boundary had to be crossed per batch, so the
protocol was the work. These read a directory of files and print a verdict, so
the whole script can run inside the image with the tree mounted at its own
path, and nothing about them changes.

Call `inside_or_reexec()` **before importing PIL**, so that a machine without
Pillow re-execs into the image rather than dying on the import.

  GHOTI_ORACLE_MODE=host    run here, against this machine's Pillow
  GIMG_ORACLE_REQUIRED=0    run here when the image cannot be reached, rather
                            than failing - the same opt-out the gtest sentinels
                            print, spelled the same way
"""

import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
_EXEC = os.path.join(_ROOT, "tools", "oracle", "oracle-exec")

# How a process knows it is already inside the image.
#
# Not an environment variable, which is what this was first written as and is
# wrong for a reason worth keeping: **`docker run` does not carry the host's
# environment in**. So the marker was set, the container started without it,
# the script re-execed itself a second time, and the failure it printed was
# "docker is not on PATH" - which is true inside the image and names the engine
# for what is a plumbing mistake one level up. A file that only the image has
# cannot be lost at the boundary the same way.
_MARKER_FILE = "/usr/local/bin/image-oracle-version"


def inside_or_reexec(reference="pillow", scratch=()):
    """Return once this process is the one that should do the work.

    In container mode and not yet inside, this replaces the process with the
    same script running in the image and never returns. `scratch` names the
    directories the script writes to; it reads the tree read-only otherwise.
    """
    if os.path.exists(_MARKER_FILE):
        return
    if os.environ.get("GHOTI_ORACLE_MODE", "container") == "host":
        return
    if not os.path.isfile(_EXEC):
        _refuse("tools/oracle/oracle-exec is missing")
        return
    argv = [_EXEC]
    for path in scratch:
        # A directory that does not exist is the caller's mistake and
        # oracle-exec says so; passing it through rather than creating it here
        # keeps that true.
        argv += ["--scratch", os.path.abspath(path)]
    argv += [reference, "--", sys.executable or "python3",
             os.path.abspath(sys.argv[0])] + sys.argv[1:]
    try:
        os.execvp(argv[0], argv)
    except OSError as exc:
        _refuse("could not run oracle-exec: %s" % exc)


def _refuse(reason):
    """Fail closed, unless the caller has asked for the lenient run by name."""
    if os.environ.get("GIMG_ORACLE_REQUIRED") == "0":
        sys.stderr.write(
            "oracle: running against this machine's own decoder, because "
            "GIMG_ORACLE_REQUIRED=0 (%s)\n" % reason)
        return
    sys.stderr.write(
        "oracle: %s\n"
        "This script's reference is pinned in "
        "tools/oracle/containers/IMAGES and built by `make oracle-build`.\n"
        "To run it against this machine's own decoder instead, say so:\n"
        "  GHOTI_ORACLE_MODE=host %s\n" % (reason, " ".join(sys.argv)))
    sys.exit(2)
