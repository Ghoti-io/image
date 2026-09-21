/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Bit-depth conversion: 8↔12↔16 bits per sample.
 * Library first-class API; codecs use these when raster depth differs from
 * codec precision.
 *
 * One rule throughout: a sample is a fraction of its range, so widening
 * replicates high bits (0 stays 0, the maximum becomes the new maximum) and
 * narrowing scales with rounding.  Each narrowing is the exact inverse of the
 * matching widening, so depth -> wider -> depth returns the original sample for
 * every value.  This is the same convention PNG 1.2 section 13.12 describes for
 * its own sample depth scaling.  Left-justifying instead (v << 4) is a
 * different convention - it treats the low bits as padding rather than as
 * value - and mixing the two, as this file used to, means a caller cannot tell
 * which they are getting.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/bitdepth.h>

// 8 → 12: replicate bits so 0..255 maps to 0..4095. (v*4095+127)/255 or
// (v<<4)|(v>>4) gives good distribution.
GIMG_API uint16_t gimg_bitdepth_8_to_12(uint8_t v) {
  return (uint16_t)(((uint16_t)v << 4) | (uint16_t)(v >> 4));
}

// 8 → 16: replicate bits so 0..255 maps to 0..65535. (v<<8)|v.
GIMG_API uint16_t gimg_bitdepth_8_to_16(uint8_t v) {
  return (uint16_t)(((uint16_t)v << 8) | (uint16_t)v);
}

// 12 → 8: scale onto the destination range, rounding to nearest -
// round(v * 255 / 4095).  This one was already right; 16_to_8 and 16_to_12
// now follow it.
GIMG_API uint8_t gimg_bitdepth_12_to_8(uint16_t v) {
  if (v >= 4095u) {
    return 255;
  }
  return (uint8_t)(((uint32_t)v * 255u + 2047u) / 4095u);
}

// 12 → 16: replicate the high bits so 0..4095 maps onto the full 0..65535,
// the same rule 8_to_12 and 8_to_16 above use.  A plain v << 4 left-justifies
// instead: it can never produce 65535, so 12-bit white decoded to 65520 and
// 8 -> 12 -> 16 landed two steps from where 8 -> 16 landed.  Bit replication is
// the usual spelling for widening a sample (PNG 1.2 section 13.12 gives the
// same rule for its own depth conversions) and it keeps the endpoints exact:
// 0 -> 0, 4095 -> 65535.
GIMG_API uint16_t gimg_bitdepth_12_to_16(uint16_t v) {
  uint16_t x = (uint16_t)(v & 0x0FFFu);
  return (uint16_t)((uint16_t)(x << 4) | (uint16_t)(x >> 8));
}

// 16 → 8: scale onto the destination range, rounding to nearest -
// round(v * 255 / 65535) - which is the exact inverse of the replicating
// widener above, so 8 -> 16 -> 8 returns the original sample for all 256
// values.  The previous (v + 128) >> 8 is the inverse of left-justification
// instead and disagreed for 127 of them: 8_to_16(128) is 32896, and
// (32896 + 128) >> 8 is 129, not 128.  Same shape as 12_to_8 below.
GIMG_API uint8_t gimg_bitdepth_16_to_8(uint16_t v) {
  return (uint8_t)(((uint32_t)v * 255u + 32767u) / 65535u);
}

// 16 → 12: scale onto the destination range, rounding to nearest -
// round(v * 4095 / 65535) - the exact inverse of the replicating widener, so
// 12 -> 16 -> 12 returns the original sample for all 4096 values.  (v + 8) >> 4
// is the inverse of left-justification and lost 2047 of them.
GIMG_API uint16_t gimg_bitdepth_16_to_12(uint16_t v) {
  return (uint16_t)(((uint32_t)v * 4095u + 32767u) / 65535u);
}
