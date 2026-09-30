#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Rate/distortion sweep: our stub lossy WebP vs cwebp on a PNG corpus.

Host side: encode each opaque PNG with encode_webp_lossy into a scratch dir.
Oracle side (libwebp image): cwebp -q 75 -m 0, dwebp -pam both, PSNR-RGB vs PNG.

Exit 0 when every file encoded and decoded. Does not fail on worse PSNR or
size — Phase G first slice is plumbing, not a quality bar.

Usage:  python3 tests/data/webp/verify_webp_rd.py
        GIMG_WEBP_ENCODE points at encode_webp_lossy (or --encode PATH).
        Corpus: third_party/webp-rd/ (tools/oracle/fetch.sh webp-rd).

PSNR is computed on RGB channels only over pixels whose reference alpha is
255 (fully opaque). Alpha PNGs in the corpus are skipped with a note.
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


def has_non_opaque_alpha(rgba: bytes) -> bool:
    for i in range(3, len(rgba), 4):
        if rgba[i] != 255:
            return True
    return False


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


def host_encode(pngs: list[str], corpus: str, work: str, encode_bin: str) -> tuple[list[str], list[str], list[str]]:
    """Encode ours on the host. Returns (opaque_rels, skipped, failures)."""
    opaque: list[str] = []
    skipped: list[str] = []
    failures: list[str] = []
    for png in pngs:
        rel = os.path.relpath(png, corpus)
        try:
            _w, _h, ref = load_png_rgba(png)
        except Exception as exc:  # noqa: BLE001
            failures.append("%s: load PNG: %s" % (rel, exc))
            continue
        if has_non_opaque_alpha(ref):
            skipped.append("%s (non-opaque alpha; lossy ALPH not yet)" % rel)
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


def measure(corpus: str, work: str) -> int:
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
        rows.append((rel, bytes_ours, bytes_cwebp, psnr_ours, psnr_cwebp))

    print(
        "name,bytes_ours,bytes_cwebp,psnr_ours,psnr_cwebp"
        "  # PSNR-RGB vs PNG; cwebp -q 75 -m 0"
    )

    def fmt(p: float) -> str:
        if math.isinf(p):
            return "inf"
        if math.isnan(p):
            return "nan"
        return "%.2f" % p

    for rel, bo, bc, po, pc in rows:
        print("%s,%d,%d,%s,%s" % (rel, bo, bc, fmt(po), fmt(pc)))

    if skipped:
        print("# skipped:")
        for s in skipped:
            print("#   %s" % s)
    if failures:
        print("# failures:", file=sys.stderr)
        for f in failures:
            print("#   %s" % f, file=sys.stderr)
        return 1

    print("# ok: %d files encoded and decoded" % len(rows))
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

    return measure(args.corpus, work)


if __name__ == "__main__":
    sys.exit(main())
