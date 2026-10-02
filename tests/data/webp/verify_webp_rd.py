#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Rate/distortion gate: our lossy WebP vs cwebp at matched size, on a corpus.

Host side: encode each PNG with encode_webp_lossy into a scratch dir.
Oracle side (libwebp image): cwebp, dwebp -pam, PSNR-RGB against the PNG.

**The bar is stated at matched size, which is the whole reason this target can
have a bar at all.** Bytes and quality trade against each other, so neither
column alone is a bar: an encoder passes a PSNR floor by spending bytes and a
byte ceiling by dropping quality, and a gate on one axis rewards exactly that.
What this does instead is ask cwebp for the best quality it can reach *without
using more bytes than we did*, by binary-searching its own -q for the largest
output no larger than ours, and then compares a single number. A deficit then
means the encoder is behind at the same price, with nothing left to trade.

Two bars, because they fail for different reasons:

  --max-deficit   per file; catches one image going wrong
  --min-mean      over the corpus; catches quality eroding everywhere at once

Measured on 2026-10-02 over the 12-file corpus, against cwebp -m 0 from
libwebp 1.5.0: mean +2.20 dB, worst -0.78 dB (pillow/bw_gradient.png, a
256x10 gradient that fits in 102 bytes). The defaults leave headroom on both.
The matched point lands within 2.3% of our byte count on every file and
within 0.5% on nine of twelve; a file whose match is further off than
--size-tol is a failure rather than a row, because the bar cannot be stated
for it.

`--no-bar` restores the report-only behaviour this had before the bar existed.
The q 75 columns are kept beside the matched ones: they are the operating
point a caller actually gets, and they are what the matched search is unable
to tell you.

Exit 0 when every file encoded, decoded and cleared both bars.

Usage:  python3 tests/data/webp/verify_webp_rd.py
        GIMG_WEBP_ENCODE points at encode_webp_lossy (or --encode PATH).
        Corpus: third_party/webp-rd/ (tools/oracle/fetch.sh webp-rd).

PSNR is computed on RGB channels only over pixels whose reference alpha is
255 (fully opaque). A PNG with a partial alpha plane is measured on those
pixels; the transparent ones are left out of the score.
"""

from __future__ import annotations

import argparse
import math
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))
DEFAULT_CORPUS = os.path.join(ROOT, "third_party", "webp-rd")
_MARKER = "/usr/local/bin/image-oracle-version"

# The bar. Set from the 2026-10-02 measurement in the module docstring, with
# headroom, and stated against an external reference at matched size rather
# than against this encoder's own history - so tightening it is a decision
# about the encoder and not a decision about the corpus.
DEFAULT_MAX_DEFICIT = 1.5  # dB, per file
DEFAULT_MIN_MEAN = 1.0     # dB, mean over the corpus
DEFAULT_SIZE_TOL = 3.0     # percent; past this the file has no matched point


def find_encode(explicit: str | None) -> str:
    if explicit:
        return explicit
    env = os.environ.get("GIMG_WEBP_ENCODE")
    if env:
        return env
    for rel in (
        "build/linux/release/apps/encode_webp_lossy",
        "build/linux/debug/apps/encode_webp_lossy",
    ):
        path = os.path.join(ROOT, rel)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    sys.stderr.write(
        "encode_webp_lossy not found; build with `make webp-encode-lossy` "
        "or set GIMG_WEBP_ENCODE\n"
    )
    sys.exit(2)


def list_pngs(corpus: str) -> list[str]:
    out: list[str] = []
    for dirpath, _dns, filenames in os.walk(corpus):
        for name in sorted(filenames):
            if name.endswith(".png"):
                out.append(os.path.join(dirpath, name))
    return out


def load_png_rgba(path: str):
    from PIL import Image

    im = Image.open(path)
    im = im.convert("RGBA")
    w, h = im.size
    return w, h, im.tobytes()


def read_pam_rgba(path: str) -> tuple[int, int, bytes]:
    data = open(path, "rb").read()
    end = data.find(b"ENDHDR\n")
    if end < 0:
        raise ValueError("no ENDHDR in %s" % path)
    hdr = data[: end + 7].decode("ascii", errors="replace")
    w = h = None
    for line in hdr.splitlines():
        if line.startswith("WIDTH "):
            w = int(line.split()[1])
        elif line.startswith("HEIGHT "):
            h = int(line.split()[1])
    if w is None or h is None:
        raise ValueError("missing WIDTH/HEIGHT in %s" % path)
    off = end + 7
    expect = w * h * 4
    if len(data) < off + expect:
        raise ValueError("truncated PAM %s" % path)
    return w, h, data[off : off + expect]


def psnr_rgb(ref: bytes, dec: bytes, w: int, h: int) -> float:
    """PSNR over RGB of opaque reference pixels. inf if identical."""
    assert len(ref) == len(dec) == w * h * 4
    sse = 0.0
    n = 0
    for i in range(0, len(ref), 4):
        if ref[i + 3] != 255:
            continue
        for c in range(3):
            d = ref[i + c] - dec[i + c]
            sse += d * d
            n += 1
    if n == 0:
        return float("nan")
    if sse == 0.0:
        return float("inf")
    mse = sse / n
    return 10.0 * math.log10(255.0 * 255.0 / mse)


def run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True)


def stem(rel: str) -> str:
    return rel.replace(os.sep, "_").rsplit(".", 1)[0]


def cwebp_encode(png: str, out: str, q: int) -> int | None:
    """Encode with cwebp at integer quality @a q. Returns bytes, or None."""
    r = run(["cwebp", "-quiet", "-q", str(q), "-m", "0", png, "-o", out])
    if r.returncode != 0:
        return None
    return os.path.getsize(out)


def cwebp_size_matched(png: str, base: str, work: str, budget: int):
    """Largest cwebp -m 0 output that is no bigger than @a budget bytes.

    Binary search over integer -q, which is monotonic in size in practice and
    is checked rather than assumed: the search keeps the largest size it has
    actually seen at or below the budget, so a non-monotonic step costs
    tightness and cannot produce a point that is over budget.

    Returns (q, bytes, path), or None when even q 0 is already larger than the
    budget - which means cwebp cannot reach our size at all and there is no
    point to compare against. That is a pass, and it is reported as one.
    """
    out = os.path.join(work, base + ".match.webp")
    b0 = cwebp_encode(png, out, 0)
    if b0 is None:
        raise RuntimeError("cwebp -q 0 failed on %s" % png)
    if b0 > budget:
        return None
    best = (0, b0)
    lo, hi = 1, 100
    while lo <= hi:
        mid = (lo + hi) // 2
        b = cwebp_encode(png, out, mid)
        if b is None:
            raise RuntimeError("cwebp -q %d failed on %s" % (mid, png))
        if b <= budget:
            if b >= best[1]:
                best = (mid, b)
            lo = mid + 1
        else:
            hi = mid - 1
    q, nbytes = best
    # The search overwrote `out` many times; the last write is whatever the
    # loop happened to try last, not the point we picked. Re-encode at the
    # chosen q so the file on disk is the one being decoded.
    final = os.path.join(work, base + ".match-q%d.webp" % q)
    got = cwebp_encode(png, final, q)
    if got != nbytes:
        raise RuntimeError(
            "cwebp -q %d is not reproducible on %s: %s then %s bytes"
            % (q, png, nbytes, got))
    return q, nbytes, final


def host_encode(pngs: list[str], corpus: str, work: str, encode_bin: str) -> tuple[list[str], list[str], list[str]]:
    """Encode ours on the host. Returns (opaque_rels, skipped, failures)."""
    opaque: list[str] = []
    skipped: list[str] = []
    failures: list[str] = []
    for png in pngs:
        rel = os.path.relpath(png, corpus)
        try:
            _w, _h, _ref = load_png_rgba(png)
        except Exception as exc:  # noqa: BLE001
            failures.append("%s: load PNG: %s" % (rel, exc))
            continue
        ours_webp = os.path.join(work, stem(rel) + ".ours.webp")
        enc = run([encode_bin, png, ours_webp])
        if enc.returncode != 0:
            failures.append(
                "%s: encode_webp_lossy: %s"
                % (rel, (enc.stderr or enc.stdout or "").strip())
            )
            continue
        opaque.append(rel)
    # Persist the opaque list for the oracle half.
    with open(os.path.join(work, "opaque.txt"), "w", encoding="utf-8") as f:
        for rel in opaque:
            f.write(rel + "\n")
    with open(os.path.join(work, "skipped.txt"), "w", encoding="utf-8") as f:
        for s in skipped:
            f.write(s + "\n")
    return opaque, skipped, failures


def measure(corpus: str, work: str, bar: bool, max_deficit: float,
        min_mean: float, size_tol: float) -> int:
    opaque_path = os.path.join(work, "opaque.txt")
    if not os.path.isfile(opaque_path):
        sys.stderr.write("missing %s (host encode did not run)\n" % opaque_path)
        return 2
    opaque = [
        line.strip()
        for line in open(opaque_path, encoding="utf-8")
        if line.strip()
    ]
    skipped = []
    skipped_path = os.path.join(work, "skipped.txt")
    if os.path.isfile(skipped_path):
        skipped = [
            line.strip()
            for line in open(skipped_path, encoding="utf-8")
            if line.strip()
        ]

    rows: list[tuple] = []
    failures: list[str] = []

    for rel in opaque:
        png = os.path.join(corpus, rel)
        base = stem(rel)
        ours_webp = os.path.join(work, base + ".ours.webp")
        cwebp_webp = os.path.join(work, base + ".cwebp.webp")
        ours_pam = os.path.join(work, base + ".ours.pam")
        cwebp_pam = os.path.join(work, base + ".cwebp.pam")

        try:
            w, h, ref = load_png_rgba(png)
        except Exception as exc:  # noqa: BLE001
            failures.append("%s: load PNG: %s" % (rel, exc))
            continue

        if not os.path.isfile(ours_webp):
            failures.append("%s: missing host encode %s" % (rel, ours_webp))
            continue
        bytes_ours = os.path.getsize(ours_webp)

        cw = run(["cwebp", "-q", "75", "-m", "0", png, "-o", cwebp_webp])
        if cw.returncode != 0:
            failures.append(
                "%s: cwebp: %s" % (rel, (cw.stderr or cw.stdout or "").strip())
            )
            continue
        bytes_cwebp = os.path.getsize(cwebp_webp)

        failed_dwebp = False
        for webp_path, pam_path, label in (
            (ours_webp, ours_pam, "dwebp ours"),
            (cwebp_webp, cwebp_pam, "dwebp cwebp"),
        ):
            dw = run(["dwebp", webp_path, "-pam", "-o", pam_path])
            if dw.returncode != 0:
                failures.append(
                    "%s: %s: %s"
                    % (rel, label, (dw.stderr or dw.stdout or "").strip())
                )
                failed_dwebp = True
                break
        if failed_dwebp:
            continue

        try:
            ow, oh, ours_rgba = read_pam_rgba(ours_pam)
            cw_w, cw_h, cwebp_rgba = read_pam_rgba(cwebp_pam)
        except ValueError as exc:
            failures.append("%s: pam: %s" % (rel, exc))
            continue
        if (ow, oh) != (w, h) or (cw_w, cw_h) != (w, h):
            failures.append(
                "%s: size mismatch ref=%dx%d ours=%dx%d cwebp=%dx%d"
                % (rel, w, h, ow, oh, cw_w, cw_h)
            )
            continue
        psnr_ours = psnr_rgb(ref, ours_rgba, w, h)
        psnr_cwebp = psnr_rgb(ref, cwebp_rgba, w, h)

        # The matched point: the most cwebp can do inside our byte count.
        try:
            match = cwebp_size_matched(png, base, work, bytes_ours)
        except RuntimeError as exc:
            failures.append("%s: %s" % (rel, exc))
            continue
        if match is None:
            q_m, bytes_m, psnr_m = -1, 0, float("nan")
        else:
            q_m, bytes_m, match_webp = match
            match_pam = os.path.join(work, base + ".match.pam")
            dw = run(["dwebp", match_webp, "-pam", "-o", match_pam])
            if dw.returncode != 0:
                failures.append(
                    "%s: dwebp matched: %s"
                    % (rel, (dw.stderr or dw.stdout or "").strip()))
                continue
            try:
                mw, mh, match_rgba = read_pam_rgba(match_pam)
            except ValueError as exc:
                failures.append("%s: matched pam: %s" % (rel, exc))
                continue
            if (mw, mh) != (w, h):
                failures.append(
                    "%s: matched size mismatch ref=%dx%d got=%dx%d"
                    % (rel, w, h, mw, mh))
                continue
            psnr_m = psnr_rgb(ref, match_rgba, w, h)
        rows.append((rel, bytes_ours, bytes_cwebp, psnr_ours, psnr_cwebp,
            q_m, bytes_m, psnr_m))

    print(
        "name,bytes_ours,bytes_cwebp,psnr_ours,psnr_cwebp,"
        "q_match,bytes_match,psnr_match,delta_db"
        "  # PSNR-RGB vs PNG; cwebp -q 75 -m 0; match = best cwebp -m 0"
        " within bytes_ours"
    )

    def fmt(p: float) -> str:
        if math.isinf(p):
            return "inf"
        if math.isnan(p):
            return "nan"
        return "%.2f" % p

    for rel, bo, bc, po, pc, qm, bm, pm in rows:
        print("%s,%d,%d,%s,%s,%s,%d,%s,%s"
            % (rel, bo, bc, fmt(po), fmt(pc),
                "none" if qm < 0 else str(qm), bm, fmt(pm),
                "n/a" if qm < 0 else fmt(po - pm)))

    if skipped:
        print("# skipped:")
        for s in skipped:
            print("#   %s" % s)
    if failures:
        print("# failures:", file=sys.stderr)
        for f in failures:
            print("#   %s" % f, file=sys.stderr)
        return 1

    # The bar. Everything above is the measurement; this is the verdict.
    violations: list[str] = []
    deltas: list[float] = []
    unmatched: list[str] = []
    for rel, bo, _bc, po, _pc, qm, bm, pm in rows:
        if qm < 0:
            # cwebp cannot reach our byte count even at q 0, so there is no
            # point inside our budget to be behind. Counted, not scored: it
            # would otherwise be a free dB in the mean.
            unmatched.append("%s (cwebp q0 is already over %d bytes)" % (rel, bo))
            continue
        gap = 100.0 * (bm - bo) / bo if bo else 0.0
        if abs(gap) > size_tol:
            # Two different facts land here and the message has to separate
            # them. A mid-range q means cwebp's size steps are coarse on this
            # file and the search could not land closer. q 100 means cwebp is
            # saturated: at the top of its quality range it still spends fewer
            # bytes than we do, which is a statement about this encoder and
            # not about the search. Either way the matched comparison cannot
            # be stated, so neither is scored and both fail.
            why = ("cwebp is saturated at -q 100 and still smaller"
                if qm >= 100 else
                "cwebp's size steps are too coarse here to land closer")
            violations.append(
                "%s: matched point is %+.1f%% of our size, past the %.1f%% "
                "tolerance (%s) - the bar cannot be stated at matched size "
                "for this file" % (rel, gap, size_tol, why))
            continue
        d = po - pm
        if math.isnan(d):
            violations.append(
                "%s: PSNR is not a number (no opaque pixels to score?), so "
                "this file is unscored rather than passing" % rel)
            continue
        deltas.append(d)
        if d < -max_deficit:
            violations.append(
                "%s: %.2f dB behind cwebp -q %d at %+.1f%% of our bytes "
                "(deficit %.2f dB, bar %.2f)"
                % (rel, -d, qm, gap, -d, max_deficit))

    mean = sum(deltas) / len(deltas) if deltas else float("nan")
    worst = min(deltas) if deltas else float("nan")
    print("# matched-size delta over %d scored files: mean %s dB, worst %s dB"
        % (len(deltas), fmt(mean), fmt(worst)))
    for u in unmatched:
        print("# unmatched (not scored): %s" % u)
    if not bar:
        print("# ok: %d files encoded and decoded; --no-bar, nothing enforced"
            % len(rows))
        return 0
    if not deltas:
        print("# no file could be scored at matched size, so the bar was "
              "applied to nothing", file=sys.stderr)
        return 1
    if mean < min_mean:
        violations.append(
            "mean matched-size delta %.2f dB is under the %.2f dB bar over "
            "%d files" % (mean, min_mean, len(deltas)))
    if violations:
        print("# quality bar failed:", file=sys.stderr)
        for v in violations:
            print("#   %s" % v, file=sys.stderr)
        return 1
    print("# ok: %d files encoded and decoded; mean %+.2f dB (bar %+.2f) and "
          "no file worse than %.2f dB behind (bar %.2f) against cwebp -m 0 at "
          "matched size" % (len(rows), mean, min_mean, -worst, max_deficit))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--corpus",
        default=DEFAULT_CORPUS,
        help="PNG corpus root (default: third_party/webp-rd)",
    )
    ap.add_argument("--encode", default=None, help="path to encode_webp_lossy")
    ap.add_argument(
        "--workdir",
        default=None,
        help="scratch directory (persists across oracle re-exec)",
    )
    ap.add_argument(
        "--max-deficit",
        type=float,
        default=DEFAULT_MAX_DEFICIT,
        help="dB a single file may be behind cwebp at matched size "
             "(default: %(default)s)",
    )
    ap.add_argument(
        "--min-mean",
        type=float,
        default=DEFAULT_MIN_MEAN,
        help="dB the corpus mean must be ahead by (default: %(default)s)",
    )
    ap.add_argument(
        "--size-tol",
        type=float,
        default=DEFAULT_SIZE_TOL,
        help="percent the matched point may differ from our byte count "
             "before the file counts as unmatchable (default: %(default)s)",
    )
    ap.add_argument(
        "--no-bar",
        action="store_true",
        help="measure and print, enforce nothing (the pre-bar behaviour)",
    )
    args = ap.parse_args()

    if not os.path.isdir(args.corpus):
        sys.stderr.write(
            "corpus missing: %s (run tools/oracle/fetch.sh webp-rd)\n"
            % args.corpus
        )
        return 2

    work = args.workdir
    if not work:
        work = tempfile.mkdtemp(prefix="webp-rd-", dir="/tmp")
    os.makedirs(work, exist_ok=True)

    inside = os.path.exists(_MARKER)
    if not inside:
        encode_bin = find_encode(args.encode)
        pngs = list_pngs(args.corpus)
        if not pngs:
            sys.stderr.write("no PNGs under %s\n" % args.corpus)
            return 2
        _opaque, _skipped, failures = host_encode(
            pngs, args.corpus, work, encode_bin
        )
        if failures:
            for f in failures:
                sys.stderr.write("%s\n" % f)
            return 1
        # Re-exec into the libwebp oracle for cwebp/dwebp (+ Pillow).
        from oracle_reexec import inside_or_reexec  # noqa: WPS433

        # Ensure argv carries workdir so the re-exec finds our encodes.
        if "--workdir" not in sys.argv:
            sys.argv.extend(["--workdir", work])
        inside_or_reexec("libwebp", scratch=[work, args.corpus])
        # never returns in container mode

    return measure(args.corpus, work, not args.no_bar, args.max_deficit,
        args.min_mean, args.size_tol)


if __name__ == "__main__":
    sys.exit(main())
