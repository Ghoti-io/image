#!/bin/sh
# Seed the fuzz corpus from the generated test images.
#
# tests/fuzz/corpus holds the inputs libFuzzer discovered on its own, which are
# the product of a randomised search and cannot be regenerated.  The valid
# images it starts from can be, so they are not stored twice: run this to copy
# them in before a fuzzing session.
#
# Generate the test images first if they are not present:
#   python3 tests/data/png/generate.py
#   python3 tests/data/jpeg/generate.py
#
# After a long session, minimise what has accumulated before committing it -
# -merge=1 keeps only the inputs that still add coverage, and every harness has
# to get a say because they share one corpus directory:
#
#   mkdir -p /tmp/cmin
#   for h in fuzz_png_load fuzz_png_encode fuzz_jpeg_load fuzz_jpeg_encode; do
#     ./build/linux/release/apps/$h -merge=1 /tmp/cmin tests/fuzz/corpus
#   done
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
corpus="$root/tests/fuzz/corpus"
mkdir -p "$corpus"
n=0
for f in "$root"/tests/data/png/*.png "$root"/tests/data/jpeg/*.jpg; do
  [ -e "$f" ] || continue
  cp "$f" "$corpus/" && n=$((n + 1))
done
echo "seeded $n file(s) into $corpus"
