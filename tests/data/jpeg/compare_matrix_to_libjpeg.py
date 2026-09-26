#!/usr/bin/env python3
"""Decode every file with our decoder and with the pinned Pillow; report any mismatch.

Pillow is asked as one batch, through tests/data/jpeg/decode_batch_pillow.py
running inside the pinned oracle image. This script cannot re-exec itself
there the way the verify_* scripts do, because the other half of every
comparison is our own host-built dump_jpeg_raster and a host binary is not
something the oracle image should be asked to run - so it runs our decoder
here, asks the reference once for the whole corpus, and compares.

  make oracle-build
  python3 tests/data/jpeg/generate_matrix.py /some/corpus
  python3 tests/data/jpeg/compare_matrix_to_libjpeg.py /some/corpus/*.jpg

GHOTI_ORACLE_MODE=host runs against this machine's own Pillow, which answers a
different question and is not the same corpus measurement.
"""
import sys, os, subprocess, struct, tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
ORACLE_EXEC = os.path.join(_ROOT, "tools", "oracle", "oracle-exec")
BATCH = os.path.join(_HERE, "decode_batch_pillow.py")
TOOL = os.environ.get("GIMG_DUMP", "build/linux/release/apps/dump_jpeg_raster")


def ask_pillow(paths, scratch):
    """Every file through the pinned Pillow in one process.

    Returns {index: (w, h, rgb_path)} for the files it read, plus a dict of
    the ones it refused. Raises SystemExit if the reference did not answer:
    this script's whole claim is a comparison against a named reference, and a
    run that did not reach one has not made it.
    """
    if os.environ.get("GHOTI_ORACLE_MODE") == "host":
        argv = [sys.executable or "python3", BATCH, scratch]
    else:
        if not os.path.isfile(ORACLE_EXEC):
            sys.exit("oracle: tools/oracle/oracle-exec is missing")
        argv = [ORACLE_EXEC, "--scratch", scratch]
        for d in sorted({os.path.dirname(os.path.abspath(p)) for p in paths}):
            argv += ["--scratch", d]
        argv += ["pillow", "--", "python3", BATCH, scratch]
    batch = "".join(os.path.abspath(p) + "\n" for p in paths)
    r = subprocess.run(argv, input=batch.encode(), capture_output=True)
    if r.returncode != 0:
        sys.exit("oracle: the pinned Pillow did not answer (exit %d):\n  %s"
                 % (r.returncode, r.stderr.decode(errors="replace").strip()))
    said = r.stdout.decode(errors="replace").splitlines()
    # The denominator, checked rather than assumed.
    count = int(said[-1]) if said and said[-1].strip().isdigit() else -1
    if count != len(paths):
        sys.exit("oracle: the reference answered %s of %d files"
                 % (count if count >= 0 else "an unreadable number", len(paths)))
    read, refused = {}, {}
    for line in said[:-1]:
        parts = line.split("\t")
        if len(parts) >= 5 and parts[1] == "ok":
            read[int(parts[0])] = (int(parts[2]), int(parts[3]), parts[4])
        elif len(parts) >= 3:
            refused[int(parts[0])] = parts[2]
    return read, refused


def main():
    files = sorted(sys.argv[1:])
    if not files:
        sys.exit("usage: compare_matrix_to_libjpeg.py <file.jpg>...")
    fail_decode = []; mismatch = []; exact = 0; ref_refused = 0
    with tempfile.TemporaryDirectory(prefix="gimg-matrix-") as scratch:
        read, refused = ask_pillow(files, scratch)
        env = dict(os.environ); env["GIMG_JPEG_FANCY_UPSAMPLE"] = "1"
        for index, p in enumerate(files):
            if index in refused:
                ref_refused += 1
                continue
            if index not in read:
                fail_decode.append((os.path.basename(p), "oracle said nothing"))
                continue
            rw, rh, rgb_path = read[index]
            r = subprocess.run([TOOL, p], capture_output=True, env=env)
            if r.returncode != 0:
                fail_decode.append(
                    (os.path.basename(p), r.stderr.decode().strip()[:60]))
                continue
            d = r.stdout
            w, h = struct.unpack("<II", d[:8]); px = d[8:]
            if (rw, rh) != (w, h):
                mismatch.append((os.path.basename(p),
                                 "size %dx%d vs %dx%d" % (w, h, rw, rh), 0))
                continue
            with open(rgb_path, "rb") as f:
                ref = f.read()
            mx = 0; n = 0
            for i in range(w * h):
                for c in range(3):
                    dd = abs(px[i * 4 + c] - ref[i * 3 + c])
                    if dd:
                        n += 1
                        if dd > mx: mx = dd
            if n == 0: exact += 1
            else: mismatch.append((os.path.basename(p),
                                   "ndiff=%d max=%d" % (n, mx), mx))
    print("%d files: %d byte-exact, %d differing, %d failed to decode, "
          "%d the reference would not read"
          % (len(files), exact, len(mismatch), len(fail_decode), ref_refused))
    for nm, why in fail_decode[:15]: print("  DECODE FAIL %s: %s" % (nm, why))
    for nm, why, mx in sorted(mismatch, key=lambda t: -t[2])[:15]:
        print("  DIFF %s: %s" % (nm, why))
    # A comparison that compared nothing is not a pass.
    if exact == 0 and not mismatch:
        sys.exit("nothing was compared: %d files in, none reached a comparison"
                 % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
