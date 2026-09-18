/**
 * @file
 *
 * Shared decode limits for the fuzz harnesses.
 *
 * A header is a handful of bytes and can name an image of any size the format's
 * fields allow, so without a cap the fuzzer spends its budget rediscovering
 * that a 200-byte file asking for 9217x14144 at 12 bits per sample needs
 * gigabytes.  That is what GIMG_Limits is for, not a defect.
 *
 * Every harness needs it, not only the one for the format being fuzzed:
 * gimg_doc_load probes the bytes and dispatches to whichever codec claims them,
 * so the PNG harnesses decode a JPEG bomb just as readily as the JPEG ones do.
 *
 * The limits go on the load as well as the decode, because a save re-decodes
 * its source item and has no decode options to pass - it honors what the
 * document was loaded with.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GIMG_TESTS_FUZZ_FUZZ_LIMITS_H
#define GIMG_TESTS_FUZZ_FUZZ_LIMITS_H

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/stream.h>

static inline const GIMG_Limits * fuzz_limits() {
  static GIMG_Limits l = {};
  l.max_decoded_pixels = 4u * 1024u * 1024u;
  return &l;
}

static inline const GIMG_Load_Options * fuzz_load_options() {
  static GIMG_Load_Options o = {};
  o.limits = fuzz_limits();
  return &o;
}

static inline const GIMG_Decode_Options * fuzz_decode_options() {
  static GIMG_Decode_Options o = {};
  o.limits = fuzz_limits();
  return &o;
}

#endif // GIMG_TESTS_FUZZ_FUZZ_LIMITS_H
