#!/bin/sh
# Reminder: the fuzz seeds are tracked as *.seed under tests/fuzz/corpus/<harness>/.
#
# The suite's rule - shared with archive, unicode, font, security and the rest -
# is that only hand-built seeds are committed. What libFuzzer writes beside them
# during a campaign is gitignored by tests/fuzz/corpus/.gitignore, so after a
# soak `git status` stays clean and nothing needs pruning before a commit.
#
# To add a seed, copy a small fixture (or a named regression input) into the
# harness directory with a .seed suffix:
#
#   cp tests/data/png/png_1x1_gray.png tests/fuzz/corpus/png_load/1x1-gray.seed
#
# Encode harnesses may also carry seeds of other formats: gimg_doc_load
# dispatches on the bytes, and the writer under test should see rasters that
# arrived from somewhere else.
#
# This script used to copy fixtures into a shared flat corpus and then ask the
# operator to minimise and commit the campaign units. That is the opposite of
# the suite rule, so it no longer does anything.
set -eu
echo "fuzz seeds are tracked as *.seed under tests/fuzz/corpus/<harness>/" >&2
echo "see tests/fuzz/README.md" >&2
exit 0
