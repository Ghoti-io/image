#!/bin/sh
#
# Download the official text of RFC 6386 into third_party/rfc6386/.
#
# The file is not committed. third_party/ is gitignored, and so is *.txt.
# What is committed is tools/webp/rfc6386.sha256, and the generator refuses
# a file that does not match it. The tables the library compiles are produced
# from this text by tools/webp/gen_vp8_tables.py.
#
# Usage:  tools/webp/fetch_rfc6386.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
url=https://www.rfc-editor.org/rfc/rfc6386.txt
dest=$root/third_party/rfc6386/rfc6386.txt
pin=$root/tools/webp/rfc6386.sha256

command -v curl >/dev/null 2>&1 || {
  printf 'missing curl, which fetch_rfc6386.sh needs\n' >&2
  exit 1
}
want=$(awk 'NF && $1 !~ /^#/ { print $1; exit }' "$pin")
mkdir -p "$(dirname "$dest")"
tmp=$dest.partial
curl -fsSL -o "$tmp" "$url"
got=$(sha256sum "$tmp" | awk '{ print $1 }')
if [ "$got" != "$want" ]; then
  printf 'RFC 6386 digest %s does not match the pin %s\n' "$got" "$want" >&2
  rm -f "$tmp"
  exit 1
fi
mv "$tmp" "$dest"
printf 'RFC 6386 is at %s\n' "$dest"
