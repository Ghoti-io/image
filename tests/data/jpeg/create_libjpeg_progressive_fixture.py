#!/usr/bin/env python3
"""
Create a libjpeg-generated progressive 8×8 grayscale JPEG for encoder comparison.

Use case: If our decoder matches libjpeg on a file encoded by cjpeg (libjpeg),
but not on the Pillow-generated progressive_8x8_gray.jpg, the 26/64 mismatch
is likely due to Pillow's progressive encoding convention (DHT placement or
refinement order) differing from libjpeg. See task doc "Encoder comparison".

Requires the pinned oracle image (`make oracle-build`) and the oracle tools
(`make oracle-tools`). cjpeg comes from the libjpeg-turbo built inside it at
the commit tools/oracle/VERSIONS names; CJPEG overrides it and
GHOTI_ORACLE_MODE=host runs this machine's own.

Steps:
  1. Write an 8×8 grayscale PPM (P5) to progressive_8x8_libjpeg.ppm.
  2. Run: cjpeg -progressive -grayscale -quality 85 -outfile progressive_8x8_libjpeg.jpg progressive_8x8_libjpeg.ppm
  3. Run: python3 verify_script_vs_libjpeg.py progressive_8x8_libjpeg.jpg [dump_jpeg_coef_ref [dump_jpeg_raster]]

Exits: 0 if cjpeg produced a valid 8×8 progressive and verify was run; 1 if cjpeg
  failed or not found; 2 if verify failed (script/decoder disagree with libjpeg).
"""
import os
import subprocess
import sys
# The reference is the pinned libjpeg-turbo in the oracle image, not whatever
# `cjpeg` this machine has. Here that is 2.1.5, from Debian's
# libjpeg-turbo-progs, and it works - which is the problem: a default that
# resolves to *something* on the machine that wrote the script is how a
# reference goes unrecorded. tools/oracle/containers/IMAGES pins 3.0.4 by
# commit under the name `libjpeg12`. CJPEG still overrides it, and
# GHOTI_ORACLE_MODE=host runs this machine's own.
#
# Note that the pinned one is a *different version* from what these scripts
# used to reach, so a fixture regenerated through it is not guaranteed to be
# the bytes already committed. None of these three run in `make test`; each is
# a hand-run tool, and the one that writes a committed fixture
# (create_libjpeg_progressive_fixture.py) should be diffed against what is in
# the tree before anything it produces is committed.
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
ORACLE_EXEC = os.path.join(_ROOT, "tools", "oracle", "oracle-exec")


def oracle_argv(inner, scratch=()):
    if os.environ.get("GHOTI_ORACLE_MODE") == "host":
        return list(inner)
    argv = [ORACLE_EXEC]
    for path in scratch:
        argv += ["--scratch", os.path.abspath(path)]
    return argv + ["libjpeg12", "--"] + list(inner)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ORACLE_DIR = os.environ.get("GIMG_JPEG_ORACLE_DIR") or SCRIPT_DIR
sys.path.insert(0, SCRIPT_DIR)


def main() -> int:
    ppm_path = os.path.join(SCRIPT_DIR, "progressive_8x8_libjpeg.ppm")
    jpeg_path = os.path.join(SCRIPT_DIR, "progressive_8x8_libjpeg.jpg")

    # 8×8 grayscale PPM (P5): magic, size, maxval, then 64 raw bytes
    # Use a simple pattern so the image is not uniform (affects coefficient structure)
    pixels = bytearray(64)
    for y in range(8):
        for x in range(8):
            pixels[y * 8 + x] = (x * 16 + y * 32) % 256
    with open(ppm_path, "wb") as f:
        f.write(b"P5\n8 8\n255\n")
        f.write(pixels)
    print(f"Wrote {ppm_path}")

    cjpeg = os.environ.get(
        "CJPEG", "/opt/libjpeg-turbo/bin/cjpeg-static")
    try:
        r = subprocess.run(
            oracle_argv([
                cjpeg,
                "-progressive",
                "-grayscale",
                "-quality",
                "85",
                "-outfile",
                os.path.abspath(jpeg_path),
                os.path.abspath(ppm_path),
            ], scratch=[os.path.dirname(os.path.abspath(jpeg_path)),
                        os.path.dirname(os.path.abspath(ppm_path))]),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except FileNotFoundError:
        print(
            "cjpeg not found. It lives in the pinned oracle image; build it "
            "with `make oracle-build`.",
            file=sys.stderr,
        )
        return 1
    if r.returncode != 0:
        print(f"cjpeg failed: {r.stderr or r.stdout}", file=sys.stderr)
        return 1
    print(f"Wrote {jpeg_path}")

    # Through oracle_path, which is the module that exists to know where the
    # oracle tools are. ORACLE_DIR falls back to this directory - where
    # compiled copies used to be committed and have not been for some time -
    # so this looked only in the one place the tool is never in, and the
    # script stopped with "Reference tool not found" naming that path.
    from oracle_path import find_oracle, BUILD_HINT
    ref_tool = find_oracle("dump_jpeg_coef_ref")
    if not ref_tool:
        print("dump_jpeg_coef_ref is not built. %s" % BUILD_HINT,
              file=sys.stderr)
        return 2
    decoder = os.environ.get(
        "GIMG_TEST_DUMP_JPEG_RASTER",
        os.path.join(SCRIPT_DIR, "..", "..", "..", "build", "linux", "release", "apps", "dump_jpeg_raster"),
    )
    if not os.path.isfile(decoder):
        decoder = os.path.join(SCRIPT_DIR, "dump_jpeg_raster")  # same dir as ref
    env = os.environ.copy()
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    # Prefer full script vs libjpeg (needs 5+ scans). Else decoder vs libjpeg first-block.
    cmd = [sys.executable, os.path.join(SCRIPT_DIR, "verify_script_vs_libjpeg.py"), jpeg_path, ref_tool]
    if decoder and os.path.isfile(decoder):
        cmd.append(decoder)
    r2 = subprocess.run(cmd, cwd=SCRIPT_DIR, env=env, capture_output=True, text=True, timeout=15)
    if r2.returncode == 0:
        print("verify_script_vs_libjpeg: script and libjpeg match (64/64).")
        return 0
    if "5+ scans" in (r2.stderr or "") or "could not build reference" in (r2.stderr or "").lower():
        # Fewer than 5 scans: compare decoder vs libjpeg only (compare_first_block_natural).
        if not decoder or not os.path.isfile(decoder):
            print("Decoder not found; cannot fall back to compare_first_block_natural.", file=sys.stderr)
            return 2
        print("Fixture has <5 scans; comparing decoder vs libjpeg first block only.")
        cmd2 = [sys.executable, os.path.join(SCRIPT_DIR, "compare_first_block_natural.py"), jpeg_path, decoder, ref_tool]
        r3 = subprocess.run(cmd2, cwd=SCRIPT_DIR, env=env, capture_output=False, timeout=15)
        return 0 if r3.returncode == 0 else 2
    print(r2.stderr or r2.stdout, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
