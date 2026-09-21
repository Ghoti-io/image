#!/bin/sh
#
# Fetch and build the reference decoders and corpora the BMP sweep uses.
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
# Both land in third_party/<name>/, which .gitignore excludes. Both are built
# here rather than only downloaded: an unbuilt bmpsuite holds no .bmp files at
# all, and an unbuilt bmplib is of no use as a decoder, so "have it" means
# "built" for each.
#
# Usage:  tools/oracle/fetch.sh [bmplib|bmpsuite|all]
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

case "$what" in
  bmplib)   fetch_bmplib ;;
  bmpsuite) fetch_bmpsuite ;;
  all)      fetch_bmplib; fetch_bmpsuite ;;
  *)        printf 'usage: tools/oracle/fetch.sh [bmplib|bmpsuite|all]\n' >&2
            exit 1 ;;
esac
