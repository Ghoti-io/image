#!/usr/bin/env python3
"""
Find a built libjpeg oracle tool.

The tools' sources are tracked in tests/tools/jpeg-oracle and `make
jpeg-oracle-tools` builds them into that directory's build/. Compiled copies
used to be committed beside the fixtures here; they are not any more, so
every caller that still looks only in this directory finds nothing.

Search order: an explicit path from the caller, honoured as given; then
GIMG_JPEG_ORACLE_DIR, the same variable the C++ tests read and the one the
make target prints; then the build directory that target writes to, so a
plain build needs no environment at all; then this directory, last and only
for compatibility with an old checkout.
"""
import os

__all__ = ["find_oracle", "repo_root"]


def repo_root() -> str:
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(4):
        if os.path.isdir(os.path.join(d, ".git")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return d


def find_oracle(name: str, explicit: str | None = None) -> str | None:
    """Return a path to oracle tool `name`, or None when it is not built.

    An explicit path is used as the caller wrote it - relative to the working
    directory, the way every other command-line path behaves. Rewriting it
    into this directory is what made passing one useless.
    """
    if explicit:
        for candidate in (explicit, explicit + ".exe"):
            if os.path.isfile(candidate):
                return os.path.abspath(candidate)
        return None
    root = repo_root()
    dirs = []
    env = os.environ.get("GIMG_JPEG_ORACLE_DIR")
    if env:
        dirs.append(env if os.path.isabs(env) else os.path.join(root, env))
    dirs.append(os.path.join(root, "tests", "tools", "jpeg-oracle", "build"))
    dirs.append(os.path.dirname(os.path.abspath(__file__)))
    for d in dirs:
        for leaf in (name, name + ".exe"):
            exe = os.path.join(d, leaf)
            if os.path.isfile(exe):
                return os.path.abspath(exe)
    return None


BUILD_HINT = (
    "Build it with: make oracle-build oracle-tools "
    "(then export GIMG_JPEG_ORACLE_DIR as that target prints)"
)
