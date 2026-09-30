#!/bin/sh
#
# Fetch and build the reference decoders and corpora the sweeps use.
#
# None of it is committed. bmpsuite is another project's test suite and bmplib
# is another project's decoder; a copy here would be a snapshot that stops
# being the thing everyone else is measured against the moment it is taken.
# What *is* committed is tools/oracle/VERSIONS, because "corroborated by
# bmplib" means nothing without saying which bmplib.
#
# bmplib is LGPL/GPL. It is built as a shared library and used through the
# separate process in tests/tools/bmp-oracle, never linked into this library.
#
# bmplib and bmpsuite land in third_party/<name>/, which .gitignore excludes.
# Both are built here rather than only downloaded: an unbuilt bmpsuite holds
# no .bmp files at all, and an unbuilt bmplib is of no use as a decoder, so
# "have it" means "built" for each.
#
# ICO refs are single blobs (Pillow pillow.ico, Wine blank.ico) fetched at the
# commit VERSIONS names into third_party/ico-refs/. No build step: the file
# is the fixture.
#
# WebP refs are five outside corpora under third_party/webp-refs/: two git
# checkouts (libwebp-test-data; codec-corpus sparse to webp-conformance/) and
# three sets of raw .webp blobs (Pillow, image-rs, golang.org/x/image), all
# at the commits VERSIONS names. Not committed; see VERSIONS for licenses.
#
# WebP RD sources are lossless PNGs under third_party/webp-rd/ (Pillow and
# image-rs blobs at the pillow-rd / image-rs-rd pins). Used by make webp-rd;
# not committed.
#
# Usage:  tools/oracle/fetch.sh [bmplib|bmpsuite|ico-refs|webp-refs|webp-rd|all]
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
what=${1:-all}

ref_for() {
  awk -v name="$1" '$1 == name { print $2; exit }' "$root/tools/oracle/VERSIONS"
}

# Clone one repository at its pinned commit into a .partial directory and only
# move it into place once it is built, so that an interrupted fetch never
# leaves something that looks ready. Parameter names avoid `dest` so a caller
# that also uses that name is not overwritten (POSIX sh has no `local`).
clone_at_ref() {
  _clone_dest=$1
  _clone_url=$2
  _clone_ref=$3
  rm -rf "$_clone_dest.partial"
  mkdir -p "$(dirname "$_clone_dest")"
  git clone --quiet --filter=blob:none --no-checkout "$_clone_url" "$_clone_dest.partial"
  git -C "$_clone_dest.partial" checkout --quiet "$_clone_ref"
}

# Run a build step quietly: on success say nothing, on failure print what it
# said before exiting. A build that works is one line of output; a build that
# breaks is all of it.
run_quiet() {
  log=$(mktemp)
  if "$@" >"$log" 2>&1; then
    rm -f "$log"
  else
    status=$?
    cat "$log" >&2
    rm -f "$log"
    exit $status
  fi
}

need() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'missing %s, which %s needs\n' "$1" "$2" >&2
    exit 1
  }
}

fetch_bmplib() {
  ref=$(ref_for bmplib)
  dest="$root/third_party/bmplib"
  if [ -f "$dest/build/libbmp.so" ] || [ -f "$dest/build/libbmp.a" ]; then
    printf 'have    third_party/bmplib\n'
    return
  fi
  need git bmplib
  need meson bmplib
  need ninja bmplib
  printf 'fetch   third_party/bmplib (%s)\n' "$ref"
  clone_at_ref "$dest" https://github.com/rupertwh/bmplib.git "$ref"
  printf 'build   third_party/bmplib\n'
  run_quiet meson setup --buildtype=release "$dest.partial/build" \
      "$dest.partial"
  run_quiet ninja -C "$dest.partial/build"
  rm -rf "$dest"
  mv "$dest.partial" "$dest"
}

fetch_bmpsuite() {
  ref=$(ref_for bmpsuite)
  dest="$root/third_party/bmpsuite"
  if [ -f "$dest/q/pal1huffmsb.bmp" ]; then
    printf 'have    third_party/bmpsuite\n'
    return
  fi
  need git bmpsuite
  need make bmpsuite
  printf 'fetch   third_party/bmpsuite (%s)\n' "$ref"
  clone_at_ref "$dest" https://github.com/jsummers/bmpsuite.git "$ref"
  # The repository holds the generator, not the images; make writes the 91
  # files into g/ q/ b/ x/.
  printf 'build   third_party/bmpsuite\n'
  run_quiet make -C "$dest.partial"
  rm -rf "$dest"
  mv "$dest.partial" "$dest"
}

# Download one raw blob at a pinned commit into dest.partial/<name>, checking
# the ICONDIR magic so a HTML error page never lands as a "fixture".
fetch_ico_blob() {
  dest_dir=$1
  name=$2
  url=$3
  ref=$4
  need curl "ico-refs ($name)"
  printf 'fetch   third_party/ico-refs/%s (%s)\n' "$name" "$ref"
  tmp="$dest_dir.partial/$name"
  mkdir -p "$dest_dir.partial"
  curl -fsSL -o "$tmp" "$url" || {
    printf 'could not download %s\n' "$url" >&2
    exit 1
  }
  # ICONDIR reserved=0, type=1 (icon) or 2 (cursor), count >= 1.
  python3 - "$tmp" "$name" <<'PY'
import struct, sys
path, name = sys.argv[1], sys.argv[2]
data = open(path, "rb").read(6)
if len(data) < 6:
    sys.stderr.write("%s: shorter than ICONDIR\n" % name)
    sys.exit(1)
reserved, typ, count = struct.unpack("<HHH", data)
if reserved != 0 or typ not in (1, 2) or count < 1:
    sys.stderr.write(
        "%s: not an ICO/CUR (reserved=%d type=%d count=%d)\n"
        % (name, reserved, typ, count))
    sys.exit(1)
PY
  printf '%s\n' "$ref" > "$dest_dir.partial/$name.ref"
}

fetch_ico_refs() {
  dest="$root/third_party/ico-refs"
  pillow_ref=$(ref_for pillow-ico)
  wine_ref=$(ref_for wine-blank-ico)
  test -n "$pillow_ref" || {
    printf 'VERSIONS has no pillow-ico line\n' >&2
    exit 1
  }
  test -n "$wine_ref" || {
    printf 'VERSIONS has no wine-blank-ico line\n' >&2
    exit 1
  }
  if [ -f "$dest/pillow.ico" ] && [ -f "$dest/wine_blank.ico" ] &&
      [ -f "$dest/pillow.ico.ref" ] && [ -f "$dest/wine_blank.ico.ref" ] &&
      [ "$(cat "$dest/pillow.ico.ref")" = "$pillow_ref" ] &&
      [ "$(cat "$dest/wine_blank.ico.ref")" = "$wine_ref" ]; then
    printf 'have    third_party/ico-refs\n'
    return
  fi
  rm -rf "$dest.partial"
  mkdir -p "$dest.partial"
  fetch_ico_blob "$dest" pillow.ico \
      "https://raw.githubusercontent.com/python-pillow/Pillow/${pillow_ref}/Tests/images/pillow.ico" \
      "$pillow_ref"
  fetch_ico_blob "$dest" wine_blank.ico \
      "https://raw.githubusercontent.com/wine-mirror/wine/${wine_ref}/dlls/shell32/resources/blank.ico" \
      "$wine_ref"
  rm -rf "$dest"
  mv "$dest.partial" "$dest"
  printf 'ready   third_party/ico-refs\n'
}

# Sparse-clone one path of a repository at its pin. Used for codec-corpus so
# the AVIF/JPEG trees never land on disk. Same naming rule as clone_at_ref.
sparse_clone_at_ref() {
  _clone_dest=$1
  _clone_url=$2
  _clone_ref=$3
  _clone_sparse=$4
  rm -rf "$_clone_dest.partial"
  mkdir -p "$(dirname "$_clone_dest")"
  git clone --quiet --filter=blob:none --sparse --no-checkout "$_clone_url" "$_clone_dest.partial"
  git -C "$_clone_dest.partial" sparse-checkout set --cone "$_clone_sparse"
  git -C "$_clone_dest.partial" checkout --quiet "$_clone_ref"
}

# Download one .webp blob at a pinned commit; refuse anything that is not a
# RIFF/WEBP container so a HTML error page never lands as a fixture.
fetch_webp_blob() {
  dest_dir=$1
  relpath=$2
  url=$3
  need curl "webp-refs ($relpath)"
  tmp="$dest_dir.partial/$relpath"
  mkdir -p "$(dirname "$tmp")"
  curl -fsSL -o "$tmp" "$url" || {
    printf 'could not download %s\n' "$url" >&2
    exit 1
  }
  python3 - "$tmp" "$relpath" <<'PY'
import sys
path, name = sys.argv[1], sys.argv[2]
data = open(path, "rb").read(12)
if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WEBP":
    sys.stderr.write("%s: not a WebP RIFF (got %r)\n" % (name, data[:12]))
    sys.exit(1)
PY
}

webp_refs_have() {
  dest=$1
  name=$2
  ref=$3
  marker="$dest/$name.ref"
  [ -f "$marker" ] && [ "$(cat "$marker")" = "$ref" ]
}

fetch_webp_refs() {
  dest="$root/third_party/webp-refs"
  ltd_ref=$(ref_for libwebp-test-data)
  cc_ref=$(ref_for codec-corpus-webp)
  pillow_ref=$(ref_for pillow-webp)
  image_rs_ref=$(ref_for image-rs-webp)
  go_ref=$(ref_for go-image-webp)
  test -n "$ltd_ref" && test -n "$cc_ref" && test -n "$pillow_ref" && \
      test -n "$image_rs_ref" && test -n "$go_ref" || {
    printf 'VERSIONS is missing one of the webp-refs pins\n' >&2
    exit 1
  }

  if webp_refs_have "$dest" libwebp-test-data "$ltd_ref" && \
      webp_refs_have "$dest" codec-corpus "$cc_ref" && \
      webp_refs_have "$dest" pillow "$pillow_ref" && \
      webp_refs_have "$dest" image-rs "$image_rs_ref" && \
      webp_refs_have "$dest" go-image "$go_ref" && \
      [ -d "$dest/libwebp-test-data" ] && \
      [ -d "$dest/codec-corpus/webp-conformance" ] && \
      [ -d "$dest/pillow" ] && \
      [ -d "$dest/image-rs" ] && \
      [ -d "$dest/go-image" ]; then
    printf 'have    third_party/webp-refs\n'
    return
  fi

  need git webp-refs
  rm -rf "$dest.partial"
  mkdir -p "$dest.partial"

  printf 'fetch   third_party/webp-refs/libwebp-test-data (%s)\n' "$ltd_ref"
  clone_at_ref "$dest.partial/libwebp-test-data" \
      https://github.com/webmproject/libwebp-test-data.git "$ltd_ref"
  # clone_at_ref leaves the tree at dest.partial/libwebp-test-data.partial;
  # normalise the name before the outer move.
  rm -rf "$dest.partial/libwebp-test-data"
  mv "$dest.partial/libwebp-test-data.partial" "$dest.partial/libwebp-test-data"
  printf '%s\n' "$ltd_ref" > "$dest.partial/libwebp-test-data.ref"

  printf 'fetch   third_party/webp-refs/codec-corpus (%s, webp-conformance)\n' "$cc_ref"
  sparse_clone_at_ref "$dest.partial/codec-corpus" \
      https://github.com/imazen/codec-corpus.git "$cc_ref" webp-conformance
  rm -rf "$dest.partial/codec-corpus"
  mv "$dest.partial/codec-corpus.partial" "$dest.partial/codec-corpus"
  printf '%s\n' "$cc_ref" > "$dest.partial/codec-corpus.ref"

  printf 'fetch   third_party/webp-refs/pillow (%s)\n' "$pillow_ref"
  for name in \
      anim_frame1.webp anim_frame2.webp \
      flower.webp flower2.webp \
      hopper.webp \
      hopper_orientation_2.webp hopper_orientation_3.webp \
      hopper_orientation_4.webp hopper_orientation_5.webp \
      hopper_orientation_6.webp hopper_orientation_7.webp \
      hopper_orientation_8.webp \
      iss634.webp transparent.webp
  do
    fetch_webp_blob "$dest" "pillow/$name" \
        "https://raw.githubusercontent.com/python-pillow/Pillow/${pillow_ref}/Tests/images/${name}"
  done
  printf '%s\n' "$pillow_ref" > "$dest.partial/pillow.ref"

  printf 'fetch   third_party/webp-refs/image-rs (%s)\n' "$image_rs_ref"
  for relpath in \
      tests/images/webp/extended_images/advertises_rgba_but_frames_are_rgb.webp \
      tests/images/webp/extended_images/anim.webp \
      tests/images/webp/extended_images/lossy_alpha.webp \
      tests/images/webp/lossless_images/2-color.webp \
      tests/images/webp/lossless_images/multi-color.webp \
      tests/images/webp/lossless_images/simple.webp \
      tests/images/webp/lossless_images/simple_xmp.webp \
      tests/images/webp/lossy_images/simple-gray.webp \
      tests/images/webp/lossy_images/simple-rgb.webp \
      tests/regression/webp/panic.webp
  do
    base=$(basename "$relpath")
    fetch_webp_blob "$dest" "image-rs/$base" \
        "https://raw.githubusercontent.com/image-rs/image/${image_rs_ref}/${relpath}"
  done
  printf '%s\n' "$image_rs_ref" > "$dest.partial/image-rs.ref"

  printf 'fetch   third_party/webp-refs/go-image (%s)\n' "$go_ref"
  for name in \
      blue-purple-pink-large.lossless.webp \
      blue-purple-pink-large.no-filter.lossy.webp \
      blue-purple-pink-large.normal-filter.lossy.webp \
      blue-purple-pink-large.simple-filter.lossy.webp \
      blue-purple-pink.lossless.webp \
      blue-purple-pink.lossy.webp \
      gopher-doc.1bpp.lossless.webp \
      gopher-doc.2bpp.lossless.webp \
      gopher-doc.4bpp.lossless.webp \
      gopher-doc.8bpp.lossless.webp \
      gopher-doc.skip-hgroup.lossless.webp \
      gopher-doc.with-alpha.lossless.webp \
      tux.lossless.webp \
      video-001.lossy.webp \
      yellow_rose.lossless.webp \
      yellow_rose.lossy-with-alpha.webp \
      yellow_rose.lossy.webp
  do
    fetch_webp_blob "$dest" "go-image/$name" \
        "https://raw.githubusercontent.com/golang/image/${go_ref}/testdata/${name}"
  done
  printf '%s\n' "$go_ref" > "$dest.partial/go-image.ref"

  rm -rf "$dest"
  mv "$dest.partial" "$dest"
  printf 'ready   third_party/webp-refs\n'
}

# Download one PNG blob at a pinned commit; refuse anything that is not a PNG
# signature so a HTML error page never lands as a fixture.
fetch_png_blob() {
  dest_dir=$1
  relpath=$2
  url=$3
  need curl "webp-rd ($relpath)"
  tmp="$dest_dir.partial/$relpath"
  mkdir -p "$(dirname "$tmp")"
  curl -fsSL -o "$tmp" "$url" || {
    printf 'could not download %s\n' "$url" >&2
    exit 1
  }
  python3 - "$tmp" "$relpath" <<'PY'
import sys
path, name = sys.argv[1], sys.argv[2]
data = open(path, "rb").read(8)
if len(data) < 8 or data[:8] != b"\x89PNG\r\n\x1a\n":
    sys.stderr.write("%s: not a PNG (got %r)\n" % (name, data[:8]))
    sys.exit(1)
PY
}

fetch_webp_rd() {
  dest="$root/third_party/webp-rd"
  pillow_ref=$(ref_for pillow-rd)
  image_rs_ref=$(ref_for image-rs-rd)
  test -n "$pillow_ref" && test -n "$image_rs_ref" || {
    printf 'VERSIONS is missing pillow-rd or image-rs-rd\n' >&2
    exit 1
  }

  if webp_refs_have "$dest" pillow "$pillow_ref" && \
      webp_refs_have "$dest" image-rs "$image_rs_ref" && \
      [ -d "$dest/pillow" ] && [ -d "$dest/image-rs" ]; then
    printf 'have    third_party/webp-rd\n'
    return
  fi

  rm -rf "$dest.partial"
  mkdir -p "$dest.partial"

  printf 'fetch   third_party/webp-rd/pillow (%s)\n' "$pillow_ref"
  for name in \
      hopper.png test-card.png caption_6_33_22.png copyleft.png \
      bw_gradient.png hopper_45.png imagedraw_floodfill_RGB.png \
      transparent.png
  do
    fetch_png_blob "$dest" "pillow/$name" \
        "https://raw.githubusercontent.com/python-pillow/Pillow/${pillow_ref}/Tests/images/${name}"
  done
  printf '%s\n' "$pillow_ref" > "$dest.partial/pillow.ref"

  printf 'fetch   third_party/webp-rd/image-rs (%s)\n' "$image_rs_ref"
  for pair in \
      "examples/fractal.png:fractal.png" \
      "examples/concat/200x300.png:200x300.png" \
      "examples/concat/300x300.png:300x300.png" \
      "examples/scaledown/scaledown-test-near.png:scaledown-test-near.png"
  do
    relpath=${pair%%:*}
    base=${pair##*:}
    fetch_png_blob "$dest" "image-rs/$base" \
        "https://raw.githubusercontent.com/image-rs/image/${image_rs_ref}/${relpath}"
  done
  printf '%s\n' "$image_rs_ref" > "$dest.partial/image-rs.ref"

  rm -rf "$dest"
  mv "$dest.partial" "$dest"
  printf 'ready   third_party/webp-rd\n'
}

case "$what" in
  bmplib)    fetch_bmplib ;;
  bmpsuite)  fetch_bmpsuite ;;
  ico-refs)  fetch_ico_refs ;;
  webp-refs) fetch_webp_refs ;;
  webp-rd)   fetch_webp_rd ;;
  all)       fetch_bmplib; fetch_bmpsuite; fetch_ico_refs; fetch_webp_refs; fetch_webp_rd ;;
  *)         printf 'usage: tools/oracle/fetch.sh [bmplib|bmpsuite|ico-refs|webp-refs|webp-rd|all]\n' >&2
             exit 1 ;;
esac
