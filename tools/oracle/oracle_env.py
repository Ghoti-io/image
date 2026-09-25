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
"""How an oracle is spelled, so that no tool here spells one itself.

**Copied from `libs/font/tools/oracle/oracle_env.py`**, which copied it from
`libs/unicode`, which landed the pattern from `notes/suite/CONTAINERS.md`. That
document is the specification; the copies are allowed to diverge and this one
does, in three ways that are written down where they occur:

  1. **Five references share one image.** Elsewhere a name is an image. Here a
     single `make test` reaches for Pillow, libjpeg, giflib, bmplib and
     bmpsuite, sometimes from one test binary, so they are one image with five
     names in `containers/IMAGES` and five arms in its version probe.

  2. **Two of the five are pinned somewhere else.** bmplib and bmpsuite were
     pinned by commit in `tools/oracle/VERSIONS` before any of this existed,
     and `fetch.sh` still reads that file. Their `IMAGES` version field is
     `@VERSIONS`, resolved here, rather than a second copy of the commit that
     could drift from the first.

  3. **The callers are mostly C++.** `unicode` and `font` are asked by Python
     differentials. Here the questions come from gtest binaries through
     `std::system`, so the argv this module computes is handed out by
     `oracle-exec` as a command line rather than imported.

The reason this library has an oracle image at all is section 4a of that
document, and it is not the reason the others do:

    An oracle can be correctly pinned and still absent, and absence is the
    failure mode that reads as success.

Modes, from GHOTI_ORACLE_MODE:

  container  (default) run the reference in the pinned image
  host                 run this machine's own copy, and print what it is
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Two directories up, so this file has to live at <repo>/tools/oracle/.
ROOT = os.path.dirname(os.path.dirname(HERE))
IMAGES = os.path.join(HERE, "containers", "IMAGES")
VERSIONS = os.path.join(HERE, "VERSIONS")

MODE = os.environ.get("GHOTI_ORACLE_MODE", "container")
ENGINE = os.environ.get("GHOTI_CONTAINER_ENGINE", "docker")


class OracleUnavailable(Exception):
    """The reference cannot be reached. Never caught into a skip."""


# How to ask each reference for its version, and what the answer must start
# with. The probe is a program *in the image* rather than a shell one-liner
# here, for the reason CONTAINERS.md section 5.6 gives: a one-liner puts a
# quoting layer between the check and the fact it checks. Two of these arms are
# compiled C programs, so that the version reported comes from the same headers
# the oracle tools are compiled against and not from a package database.
#
# In host mode the same five questions are asked by tools/oracle/
# host-oracle-version, which shares the two C files with the image.
PROBE = {
    "pillow": "Pillow ",
    "libjpeg": "libjpeg-turbo ",
    "giflib": "giflib ",
    "bmplib": "bmplib ",
    "bmpsuite": "bmpsuite ",
}

CONTAINER_PROBE = "image-oracle-version"
HOST_PROBE = os.path.join(HERE, "host-oracle-version")

_pins = None
_versions = None
_cache = {}


def versions_file():
    """tools/oracle/VERSIONS as a dict, read the way fetch.sh reads it.

    `awk '$1 == name { print $2; exit }'` - first match wins, so a duplicate
    key added earlier in the file shadows the real one. Spelled the same way
    here on purpose: two readers of one pin file that disagree about which line
    wins is the failure the shared file was meant to prevent.
    """
    global _versions
    if _versions is not None:
        return _versions
    _versions = {}
    if not os.path.exists(VERSIONS):
        return _versions
    with open(VERSIONS, "r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 2 and parts[0] not in _versions:
                _versions[parts[0]] = parts[1]
    return _versions


def pins():
    """The IMAGES table: name -> (image, version, description)."""
    global _pins
    if _pins is not None:
        return _pins
    _pins = {}
    if not os.path.exists(IMAGES):
        return _pins
    with open(IMAGES, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise OracleUnavailable(
                    "containers/IMAGES: not three tab-separated fields: %r"
                    % line)
            name, image, version = parts[0], parts[1], parts[2]
            if version == "@VERSIONS":
                # The pin lives in tools/oracle/VERSIONS. An absent key is a
                # hard error rather than an empty expectation, because an empty
                # expectation is contained in every answer and check_pin()
                # would then pass against anything.
                ref = versions_file().get(name)
                if not ref:
                    raise OracleUnavailable(
                        "IMAGES says %s is pinned in tools/oracle/VERSIONS, "
                        "and VERSIONS has no %s line" % (name, name))
                version = "%s %s" % (name, ref)
            _pins[name] = (image, version, parts[3] if len(parts) > 3 else "")
    return _pins


def _engine_ok():
    if shutil.which(ENGINE) is None:
        raise OracleUnavailable(
            "%s is not on PATH, and GHOTI_ORACLE_MODE is 'container'.\n"
            "Install it, or run with GHOTI_ORACLE_MODE=host to use this "
            "machine's own decoders - which answers a different question, and "
            "says so in the line it prints." % ENGINE)


def _have_image(image):
    finished = subprocess.run([ENGINE, "image", "exists", image],
        capture_output=True)
    if finished.returncode == 0:
        return True
    # `image exists` is podman's. Fall back to a docker-portable spelling.
    finished = subprocess.run([ENGINE, "image", "inspect", image],
        capture_output=True)
    return finished.returncode == 0


def ensure(name):
    """Make the reference runnable, or raise saying what is missing."""
    if MODE == "host":
        if not os.path.isfile(HOST_PROBE):
            raise OracleUnavailable(
                "GHOTI_ORACLE_MODE=host and %s is missing" % HOST_PROBE)
        return
    if MODE != "container":
        raise OracleUnavailable("GHOTI_ORACLE_MODE=%r is not a mode" % MODE)
    _engine_ok()
    table = pins()
    if name not in table:
        raise OracleUnavailable(
            "no pin for %r in tools/oracle/containers/IMAGES" % name)
    image = table[name][0]
    if _have_image(image):
        return
    # There is nothing to pull: this image is built here, not published. Say
    # what to run rather than reaching for a registry that has never heard of
    # it - a failed pull names a network problem for what is a missing build.
    raise OracleUnavailable(
        "the oracle image is not built: %s\n"
        "Build it with:  make oracle-build\n"
        "It is built here rather than pulled, so there is no registry copy."
        % image)


# Ask one oracle's questions of a different pin, e.g.
#   GHOTI_ORACLE_ALIAS=pillow=pillow-next make check-oracle-jpeg
# Nothing uses this yet - there is one image. It is kept because the reading it
# exists for is exactly what a Pillow or libjpeg upgrade will want (the same
# fixtures against two reference versions, where a disagreement is what the
# reference changed rather than what this library got wrong), and because
# provenance() has to name the pin that *answered* either way.
ALIAS = dict(
    pair.split("=", 1)
    for pair in os.environ.get("GHOTI_ORACLE_ALIAS", "").split(",")
    if "=" in pair)


def command(name, argv, scratch=None):
    """The argv that runs `argv` against `name`'s reference.

    The repository is bind-mounted at its own host path, so any path a caller
    has already built resolves on both sides and no tool needs translating.
    That matters more here than elsewhere: the callers are gtest binaries that
    build absolute paths out of GIMG_TEST_DATA_JPEG and hand them to a shell.

    `scratch` is a directory - or a list of them - the reference must be able
    to *write*, named the same way. This library needs them: a decode oracle is
    asked to leave its pixels in a .raw file under tests/out/, and an encode
    oracle writes a whole JPEG. Passing the directory explicitly rather than
    mounting /tmp is deliberate, so that a caller which forgets to declare one
    fails on a path that does not exist instead of writing where nobody reads.

    Everything else is closed. `--network none` because no reference here has
    business reaching the network, and the tree is read-only because a corpus
    quietly edited by the thing being compared against it is not a comparison.
    """
    name = ALIAS.get(name, name)
    ensure(name)
    if MODE == "host":
        return list(argv)
    image = pins()[name][0]
    out = [ENGINE, "run", "--rm", "-i",
        "--network", "none",
        "--volume", "%s:%s:ro" % (ROOT, ROOT)]
    paths = [scratch] if isinstance(scratch, str) else list(scratch or [])
    for path in paths:
        out += ["--volume", "%s:%s:rw" % (path, path)]
    return out + ["--workdir", ROOT, image] + list(argv)


def version(name):
    """What the reference says it is. Runs it; the answer is cached."""
    name = ALIAS.get(name, name)
    key = ("version", name)
    if key in _cache:
        return _cache[key]
    expect = PROBE.get(name, "")
    probe = ([CONTAINER_PROBE, name] if MODE == "container"
             else [HOST_PROBE, name])
    # stdin=DEVNULL, and it is not a tidiness flag. `docker run -i` attaches
    # the caller's stdin, and subprocess.run() inherits it: the probe container
    # therefore drained the batch that a caller had piped in for the *real*
    # question, and the oracle read an empty corpus and exited 0. It was caught
    # by verify_resample.py checking the count the reference returned against
    # the count it sent - "the reference answered 0 of 490 cases" - which is
    # the only reason it was caught at all, because every comparison in an
    # empty batch agrees.
    finished = subprocess.run(command(name, probe), capture_output=True,
        text=True, stdin=subprocess.DEVNULL)
    # stdout only. `docker` on this machine is a podman shim that prints a
    # banner to stderr on every invocation, and a probe that reads both streams
    # reads the banner. The same trap waits for any caller that merges them:
    # the reference's answers and the engine's chatter would land on one stream
    # and the extra line would be scored as a disagreement.
    text = finished.stdout.strip().splitlines()
    text = text[0] if text else ""
    if finished.returncode != 0 or (expect and not text.startswith(expect)):
        raise OracleUnavailable(
            "%s answered %r, which does not look like a version%s"
            % (name, text,
               ("\n  " + finished.stderr.strip()) if finished.stderr.strip()
               else ""))
    _cache[key] = text
    return text


def check_pin(name):
    """Raise unless the reference's version matches containers/IMAGES.

    **In both modes**, as in `font` and unlike `unicode`. CONTAINERS.md section
    2.6 is the rule: for an image built here the run-time version check is the
    real guarantee, because two builds of one Containerfile are not two copies
    of one image and there is no digest to trust. `unicode` could relax this in
    host mode because every image it names is a stock one pinned by digest;
    every line in this library's IMAGES names the same built-here image, so the
    relaxation would leave nothing checking anything.

    What that costs is worth stating plainly: host mode passes here only on a
    machine whose Pillow, libjpeg and giflib happen to match the pins. On the
    machine this was written on they do, which is exactly why host and
    container answers can be diffed against each other - and why a machine
    where they do not is one whose green line meant something different.
    """
    name = ALIAS.get(name, name)
    table = pins()
    if name not in table:
        return version(name)
    said = table[name][1]
    got = version(name)
    if said not in got:
        raise OracleUnavailable(
            "%s: IMAGES says %s and it answers %r" % (name, said, got))
    return got


def provenance(names):
    """One line naming every reference that answered, and how.

    The name printed is the one that *answered*, not the one the caller asked
    for: under an alias those differ, and printing the requested name makes the
    line name the wrong pin.
    """
    # Not "host, unpinned": check_pin() applies in both modes here, so a host
    # run that got this far has matched the pin like any other.
    parts = []
    for name in names:
        resolved = ALIAS.get(name, name)
        label = resolved if resolved == name else "%s as %s" % (resolved, name)
        said = check_pin(name)
        # Three of the five references name themselves in their version line
        # ("giflib 5.2.2"), so printing label and version both gives "giflib
        # giflib 5.2.2". Say it once. The label still leads where they differ,
        # which is the case that matters: "pillow Pillow 11.1.0" and, under an
        # alias, "pillow-next as pillow ...".
        if said.lower().startswith(label.lower()):
            parts.append(said)
        else:
            parts.append("%s %s" % (label, said))
    return "oracle(%s): %s" % (MODE, ", ".join(parts))


def main(argv):
    """`python3 oracle_env.py <names...>` prints the provenance line.

    Used by the Makefile's oracle-verify target and by anything that wants to
    ask "can this machine reach the references, and which ones". Exit 2 rather
    than 1 when it cannot, to keep it distinct from a comparison that ran and
    disagreed.
    """
    names = argv[1:] or sorted(pins())
    try:
        sys.stdout.write(provenance(names) + "\n")
    except OracleUnavailable as exc:
        sys.stderr.write("oracle: %s\n" % exc)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
