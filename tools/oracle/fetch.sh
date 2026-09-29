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
# Usage:  tools/oracle/fetch.sh [bmplib|bmpsuite|ico-refs|all]
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
# leaves something that looks ready.
clone_at_ref() {
  dest=$1
  url=$2
  ref=$3
  rm -rf "$dest.partial"
  mkdir -p "$(dirname "$dest")"
  git clone --quiet --filter=blob:none --no-checkout "$url" "$dest.partial"
  git -C "$dest.partial" checkout --quiet "$ref"
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

case "$what" in
  bmplib)   fetch_bmplib ;;
  bmpsuite) fetch_bmpsuite ;;
  ico-refs) fetch_ico_refs ;;
  all)      fetch_bmplib; fetch_bmpsuite; fetch_ico_refs ;;
  *)        printf 'usage: tools/oracle/fetch.sh [bmplib|bmpsuite|ico-refs|all]\n' >&2
            exit 1 ;;
esac
