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
 * The ITU-T T.4 run-length code tables, and reading a run out of them.
 *
 * Two formats in this library use these codes and they are not related to
 * each other. BMP's OS/2 2.x "Huffman 1D" compression is T.4 one-dimensional
 * coding, and TIFF's CCITT Group 3 and Group 4 are T.4 and T.6 - the same
 * tables, different framing. They lived in the BMP codec until TIFF needed
 * them, and a second copy of a table of 208 codes is a second copy that can
 * be corrected once.
 *
 * What is *not* here is anything either format decided for itself: where a
 * row begins, whether lines are separated by an end-of-line, which bit of a
 * byte comes first. Those differ, and they belong to the codec that has an
 * opinion about them.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_SHARED_CCITT_H
#define GHOTI_IO_GIMG_SRC_CODEC_SHARED_CCITT_H

#include <ghoti.io/image/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** One entry of a T.4 code table: the code, its length in bits, and the run
 * length it stands for. */
typedef struct {
  uint16_t code;
  uint8_t bits;
  uint16_t run;
} gimg_ccitt_code_t;

extern const gimg_ccitt_code_t gimg_ccitt_white[];
extern const gimg_ccitt_code_t gimg_ccitt_black[];
extern const size_t gimg_ccitt_white_count;
extern const size_t gimg_ccitt_black_count;

/** Longest code in either table, which bounds how far the matcher looks. */
#define GIMG_CCITT_MAX_BITS 14

/** A run of 64 or more is a makeup code and must be followed by another. */
#define GIMG_CCITT_TERMINATING 64

/**
 * A bit reader over encoded data.
 *
 * `lsb_first` is TIFF's FillOrder 2, where the bits of a byte are numbered
 * from the least significant end. BMP has no such option and always leaves
 * it false; a TIFF that sets it and is read as though it had not produces
 * noise rather than an error, which is why it is a field here rather than a
 * caller's problem.
 */
typedef struct {
  const unsigned char * data;
  size_t size;
  size_t bit;
  bool lsb_first;
} gimg_ccitt_bits_t;

bool gimg_ccitt_read_bit(gimg_ccitt_bits_t * b, unsigned int * out);

/**
 * Read one complete run length, following makeup codes until a terminating
 * one arrives.
 *
 * @return true on success; false at the end of the data or on a bit pattern
 *   no code matches.
 */
bool gimg_ccitt_read_run(
    gimg_ccitt_bits_t * b, bool white, uint32_t * out_run);

/** The table entry for an exact run length, or NULL when there is none. */
const gimg_ccitt_code_t * gimg_ccitt_code_for(bool white, uint32_t run);

#endif // GHOTI_IO_GIMG_SRC_CODEC_SHARED_CCITT_H
