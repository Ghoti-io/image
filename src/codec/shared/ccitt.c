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
 * The T.4 code tables, shared by the BMP and TIFF codecs. See ccitt.h.
 */

#include <ghoti.io/image/macros.h>

#include "ccitt.h"

const gimg_ccitt_code_t gimg_ccitt_white[] = {
  {0x0007,  4,    2}, {0x0008,  4,    3}, {0x000B,  4,    4},
  {0x000C,  4,    5}, {0x000E,  4,    6}, {0x000F,  4,    7},
  {0x0007,  5,   10}, {0x0008,  5,   11}, {0x0012,  5,  128},
  {0x0013,  5,    8}, {0x0014,  5,    9}, {0x001B,  5,   64},
  {0x0003,  6,   13}, {0x0007,  6,    1}, {0x0008,  6,   12},
  {0x0017,  6,  192}, {0x0018,  6, 1664}, {0x002A,  6,   16},
  {0x002B,  6,   17}, {0x0034,  6,   14}, {0x0035,  6,   15},
  {0x0003,  7,   22}, {0x0004,  7,   23}, {0x0008,  7,   20},
  {0x000C,  7,   19}, {0x0013,  7,   26}, {0x0017,  7,   21},
  {0x0018,  7,   28}, {0x0024,  7,   27}, {0x0027,  7,   18},
  {0x0028,  7,   24}, {0x002B,  7,   25}, {0x0037,  7,  256},
  {0x0002,  8,   29}, {0x0003,  8,   30}, {0x0004,  8,   45},
  {0x0005,  8,   46}, {0x000A,  8,   47}, {0x000B,  8,   48},
  {0x0012,  8,   33}, {0x0013,  8,   34}, {0x0014,  8,   35},
  {0x0015,  8,   36}, {0x0016,  8,   37}, {0x0017,  8,   38},
  {0x001A,  8,   31}, {0x001B,  8,   32}, {0x0024,  8,   53},
  {0x0025,  8,   54}, {0x0028,  8,   39}, {0x0029,  8,   40},
  {0x002A,  8,   41}, {0x002B,  8,   42}, {0x002C,  8,   43},
  {0x002D,  8,   44}, {0x0032,  8,   61}, {0x0033,  8,   62},
  {0x0034,  8,   63}, {0x0035,  8,    0}, {0x0036,  8,  320},
  {0x0037,  8,  384}, {0x004A,  8,   59}, {0x004B,  8,   60},
  {0x0052,  8,   49}, {0x0053,  8,   50}, {0x0054,  8,   51},
  {0x0055,  8,   52}, {0x0058,  8,   55}, {0x0059,  8,   56},
  {0x005A,  8,   57}, {0x005B,  8,   58}, {0x0064,  8,  448},
  {0x0065,  8,  512}, {0x0067,  8,  640}, {0x0068,  8,  576},
  {0x0098,  9, 1472}, {0x0099,  9, 1536}, {0x009A,  9, 1600},
  {0x009B,  9, 1728}, {0x00CC,  9,  704}, {0x00CD,  9,  768},
  {0x00D2,  9,  832}, {0x00D3,  9,  896}, {0x00D4,  9,  960},
  {0x00D5,  9, 1024}, {0x00D6,  9, 1088}, {0x00D7,  9, 1152},
  {0x00D8,  9, 1216}, {0x00D9,  9, 1280}, {0x00DA,  9, 1344},
  {0x00DB,  9, 1408}, {0x0008, 11, 1792}, {0x000C, 11, 1856},
  {0x000D, 11, 1920}, {0x0012, 12, 1984}, {0x0013, 12, 2048},
  {0x0014, 12, 2112}, {0x0015, 12, 2176}, {0x0016, 12, 2240},
  {0x0017, 12, 2304}, {0x001C, 12, 2368}, {0x001D, 12, 2432},
  {0x001E, 12, 2496}, {0x001F, 12, 2560},
};

const gimg_ccitt_code_t gimg_ccitt_black[] = {
  {0x0002,  2,    3}, {0x0003,  2,    2}, {0x0002,  3,    1},
  {0x0003,  3,    4}, {0x0002,  4,    6}, {0x0003,  4,    5},
  {0x0003,  5,    7}, {0x0004,  6,    9}, {0x0005,  6,    8},
  {0x0004,  7,   10}, {0x0005,  7,   11}, {0x0007,  7,   12},
  {0x0004,  8,   13}, {0x0007,  8,   14}, {0x0018,  9,   15},
  {0x0008, 10,   18}, {0x000F, 10,   64}, {0x0017, 10,   16},
  {0x0018, 10,   17}, {0x0037, 10,    0}, {0x0008, 11, 1792},
  {0x000C, 11, 1856}, {0x000D, 11, 1920}, {0x0017, 11,   24},
  {0x0018, 11,   25}, {0x0028, 11,   23}, {0x0037, 11,   22},
  {0x0067, 11,   19}, {0x0068, 11,   20}, {0x006C, 11,   21},
  {0x0012, 12, 1984}, {0x0013, 12, 2048}, {0x0014, 12, 2112},
  {0x0015, 12, 2176}, {0x0016, 12, 2240}, {0x0017, 12, 2304},
  {0x001C, 12, 2368}, {0x001D, 12, 2432}, {0x001E, 12, 2496},
  {0x001F, 12, 2560}, {0x0024, 12,   52}, {0x0027, 12,   55},
  {0x0028, 12,   56}, {0x002B, 12,   59}, {0x002C, 12,   60},
  {0x0033, 12,  320}, {0x0034, 12,  384}, {0x0035, 12,  448},
  {0x0037, 12,   53}, {0x0038, 12,   54}, {0x0052, 12,   50},
  {0x0053, 12,   51}, {0x0054, 12,   44}, {0x0055, 12,   45},
  {0x0056, 12,   46}, {0x0057, 12,   47}, {0x0058, 12,   57},
  {0x0059, 12,   58}, {0x005A, 12,   61}, {0x005B, 12,  256},
  {0x0064, 12,   48}, {0x0065, 12,   49}, {0x0066, 12,   62},
  {0x0067, 12,   63}, {0x0068, 12,   30}, {0x0069, 12,   31},
  {0x006A, 12,   32}, {0x006B, 12,   33}, {0x006C, 12,   40},
  {0x006D, 12,   41}, {0x00C8, 12,  128}, {0x00C9, 12,  192},
  {0x00CA, 12,   26}, {0x00CB, 12,   27}, {0x00CC, 12,   28},
  {0x00CD, 12,   29}, {0x00D2, 12,   34}, {0x00D3, 12,   35},
  {0x00D4, 12,   36}, {0x00D5, 12,   37}, {0x00D6, 12,   38},
  {0x00D7, 12,   39}, {0x00DA, 12,   42}, {0x00DB, 12,   43},
  {0x004A, 13,  640}, {0x004B, 13,  704}, {0x004C, 13,  768},
  {0x004D, 13,  832}, {0x0052, 13, 1280}, {0x0053, 13, 1344},
  {0x0054, 13, 1408}, {0x0055, 13, 1472}, {0x005A, 13, 1536},
  {0x005B, 13, 1600}, {0x0064, 13, 1664}, {0x0065, 13, 1728},
  {0x006C, 13,  512}, {0x006D, 13,  576}, {0x0072, 13,  896},
  {0x0073, 13,  960}, {0x0074, 13, 1024}, {0x0075, 13, 1088},
  {0x0076, 13, 1152}, {0x0077, 13, 1216},
};

const size_t gimg_ccitt_white_count =
    sizeof(gimg_ccitt_white) / sizeof(gimg_ccitt_white[0]);
const size_t gimg_ccitt_black_count =
    sizeof(gimg_ccitt_black) / sizeof(gimg_ccitt_black[0]);

bool gimg_ccitt_read_bit(gimg_ccitt_bits_t * b, unsigned int * out) {
  if (b->bit >= b->size * 8u) {
    return false;
  }
  const unsigned char byte = b->data[b->bit >> 3];
  const unsigned int index = (unsigned int)(b->bit & 7u);
  *out = b->lsb_first ? ((byte >> index) & 1u)
                      : ((byte >> (7u - index)) & 1u);
  b->bit++;
  return true;
}

bool gimg_ccitt_read_run(
    gimg_ccitt_bits_t * b, bool white, uint32_t * out_run) {
  const gimg_ccitt_code_t * table =
      white ? gimg_ccitt_white : gimg_ccitt_black;
  const size_t count = white ? gimg_ccitt_white_count : gimg_ccitt_black_count;

  uint32_t total = 0;
  for (;;) {
    uint32_t code = 0;
    unsigned int bits = 0;
    bool matched = false;
    while (bits < GIMG_CCITT_MAX_BITS) {
      unsigned int bit;
      if (!gimg_ccitt_read_bit(b, &bit)) {
        return false;
      }
      code = (code << 1) | bit;
      bits++;
      for (size_t i = 0; i < count; i++) {
        if (table[i].bits == bits && table[i].code == code) {
          total += table[i].run;
          if (table[i].run < GIMG_CCITT_TERMINATING) {
            *out_run = total;
            return true;
          }
          matched = true;
          break;
        }
      }
      if (matched) {
        break;  // A makeup code; read the rest of the run.
      }
    }
    if (!matched) {
      return false;  // No code of any length matches these bits.
    }
  }
}

const gimg_ccitt_code_t * gimg_ccitt_code_for(bool white, uint32_t run) {
  const gimg_ccitt_code_t * table =
      white ? gimg_ccitt_white : gimg_ccitt_black;
  const size_t count = white ? gimg_ccitt_white_count : gimg_ccitt_black_count;
  for (size_t i = 0; i < count; i++) {
    if (table[i].run == run) {
      return &table[i];
    }
  }
  return NULL;
}
