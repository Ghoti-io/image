/**
 * @file
 *
 * CCITT Group 3 one-dimensional decoding, for OS/2 BMPs whose ulCompression
 * is 3.  Windows spells 3 BI_BITFIELDS; an OS/2 BITMAPCOREHEADER2 means this,
 * which is why the two vocabularies are normalized before anything reads the
 * number.
 *
 * The code tables are ITU-T T.4 Table 1 and Table 2, plus the extended makeup
 * codes both colours share.  A run is a makeup code, which is always a
 * multiple of 64, followed by a terminating code of 0 to 63; a run under 64
 * needs no makeup.  That is why no entry carries a "terminating" flag here:
 * a run below 64 is one by definition.
 *
 * What no specification states is which palette index T.4's "white" and
 * "black" mean.  There is no BMP document that says, and the format's own
 * author calls the documentation close to non-existent.  It is settled here by
 * measurement rather than by convention: bmpsuite stores one picture twice,
 * as q/pal1huffmsb.bmp and as g/pal1.bmp, and only black = index 1 makes the
 * two decode alike.  The test asserts that, so a future change of mind has to
 * disagree with the picture rather than with a comment.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include "bmp_internal.h"

#include <string.h>

/** One entry of a T.4 code table: the code, its length in bits, and the run
 * length it stands for. */
typedef struct {
  uint16_t code;
  uint8_t bits;
  uint16_t run;
} gimg_bmp_g31d_code_t;

static const gimg_bmp_g31d_code_t gimg_bmp_g31d_white[] = {
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

static const gimg_bmp_g31d_code_t gimg_bmp_g31d_black[] = {
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

/** Longest code in either table, which bounds how far the matcher looks. */
#define GIMG_BMP_G31D_MAX_BITS 14

/** A run of 64 or more is a makeup code and must be followed by another. */
#define GIMG_BMP_G31D_TERMINATING 64

/** MSB-first bit reader over the encoded data. */
typedef struct {
  const unsigned char * data;
  size_t size;
  size_t bit;
} gimg_bmp_g31d_bits_t;

static bool g31d_read_bit(gimg_bmp_g31d_bits_t * b, unsigned int * out) {
  if (b->bit >= b->size * 8u) {
    return false;
  }
  *out = (b->data[b->bit >> 3] >> (7u - (b->bit & 7u))) & 1u;
  b->bit++;
  return true;
}

/**
 * Read one complete run length, following makeup codes until a terminating
 * one arrives.
 *
 * @return true on success; false at the end of the data or on a bit pattern
 *   no code matches.
 */
static bool g31d_read_run(gimg_bmp_g31d_bits_t * b, bool white,
    uint32_t * out_run) {
  const gimg_bmp_g31d_code_t * table =
      white ? gimg_bmp_g31d_white : gimg_bmp_g31d_black;
  size_t count = white
      ? sizeof(gimg_bmp_g31d_white) / sizeof(gimg_bmp_g31d_white[0])
      : sizeof(gimg_bmp_g31d_black) / sizeof(gimg_bmp_g31d_black[0]);

  uint32_t total = 0;
  for (;;) {
    uint32_t code = 0;
    unsigned int bits = 0;
    bool matched = false;
    while (bits < GIMG_BMP_G31D_MAX_BITS) {
      unsigned int bit;
      if (!g31d_read_bit(b, &bit)) {
        return false;
      }
      code = (code << 1) | bit;
      bits++;
      for (size_t i = 0; i < count; i++) {
        if (table[i].bits == bits && table[i].code == code) {
          total += table[i].run;
          if (table[i].run < GIMG_BMP_G31D_TERMINATING) {
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

/**
 * Step over an end-of-line marker, and any fill bits before it.
 *
 * T.4 allows a line to be preceded by eleven or more zero bits followed by a
 * one.  A run of zeros that turns out to be shorter than that is not an EOL,
 * so the reader is put back where it started and the bits are decoded as a
 * code.
 */
static void g31d_skip_eol(gimg_bmp_g31d_bits_t * b) {
  size_t start = b->bit;
  unsigned int zeros = 0;
  for (;;) {
    unsigned int bit;
    if (!g31d_read_bit(b, &bit)) {
      b->bit = start;
      return;
    }
    if (bit) {
      break;
    }
    if (++zeros > 64u) {  // Not an EOL, and not worth scanning further.
      b->bit = start;
      return;
    }
  }
  if (zeros < 11u) {
    b->bit = start;
  }
}

GIMG_Result gimg_bmp_huffman_expand(const unsigned char * data, size_t size,
    uint32_t width, uint32_t height, size_t stride, unsigned char * out) {
  if (!data || !out || !width || !height) {
    return GIMG_ERR_INTERNAL;
  }
  gimg_bmp_g31d_bits_t bits = {data, size, 0};

  // The encoded lines are in the file's own row order, so they are written
  // straight into the packed rows an uncompressed 1-bit image would have had.
  // Everything downstream - the palette lookup, the bottom-up flip, the limits
  // - then treats this exactly like one, because after this it is one.
  memset(out, 0, stride * (size_t)height);

  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = out + ((size_t)y * stride);
    uint32_t x = 0;
    bool white = true;  // T.4 lines begin with a white run, possibly empty.
    while (x < width) {
      g31d_skip_eol(&bits);
      uint32_t run = 0;
      if (!g31d_read_run(&bits, white, &run)) {
        return GIMG_ERR_CORRUPT;
      }
      if (run > width - x) {
        // A run that overhangs the line is clipped rather than refused, the
        // same judgment the RLE decoder makes: the pixels it would have
        // written past the end have nowhere to go, and the rest of the image
        // is still readable.
        run = width - x;
      }
      if (!white) {
        for (uint32_t i = 0; i < run; i++) {
          uint32_t at = x + i;
          row[at >> 3] |= (unsigned char)(0x80u >> (at & 7u));
        }
      }
      x += run;
      white = !white;
    }
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

/** MSB-first bit writer, counting past the end rather than writing past it so
 * that a caller can size a buffer by encoding into none. */
typedef struct {
  unsigned char * data; ///< NULL to measure without writing.
  size_t capacity;
  size_t bit;
  bool overflow;
} gimg_bmp_g31d_out_t;

static void g31d_put_bits(
    gimg_bmp_g31d_out_t * o, uint32_t code, unsigned int bits) {
  for (unsigned int i = 0; i < bits; i++) {
    unsigned int bit = (code >> (bits - 1u - i)) & 1u;
    size_t byte = o->bit >> 3;
    if (o->data) {
      if (byte >= o->capacity) {
        o->overflow = true;
        return;
      }
      if ((o->bit & 7u) == 0u) {
        o->data[byte] = 0;
      }
      o->data[byte] |= (unsigned char)(bit << (7u - (o->bit & 7u)));
    }
    o->bit++;
  }
}

/** Find a code for an exact run length in one colour's table. */
static const gimg_bmp_g31d_code_t * g31d_code_for(bool white, uint32_t run) {
  const gimg_bmp_g31d_code_t * table =
      white ? gimg_bmp_g31d_white : gimg_bmp_g31d_black;
  size_t count = white
      ? sizeof(gimg_bmp_g31d_white) / sizeof(gimg_bmp_g31d_white[0])
      : sizeof(gimg_bmp_g31d_black) / sizeof(gimg_bmp_g31d_black[0]);
  for (size_t i = 0; i < count; i++) {
    if (table[i].run == run) {
      return &table[i];
    }
  }
  return NULL;
}

/**
 * Write one run as a makeup code, or several, followed by a terminating one.
 *
 * T.4 states runs of 64 and over as a makeup code carrying a multiple of 64
 * and then a terminating code of 0 to 63.  The largest makeup either colour
 * has is 2560, so a longer run takes more than one - and a terminating code is
 * always written, including for a remainder of zero, because that is what ends
 * the run.
 */
static bool g31d_put_run(
    gimg_bmp_g31d_out_t * o, bool white, uint32_t run) {
  while (run >= GIMG_BMP_G31D_TERMINATING) {
    uint32_t makeup = run - (run % GIMG_BMP_G31D_TERMINATING);
    if (makeup > 2560u) {
      makeup = 2560u;
    }
    const gimg_bmp_g31d_code_t * code = g31d_code_for(white, makeup);
    if (!code) {
      return false;
    }
    g31d_put_bits(o, code->code, code->bits);
    run -= makeup;
  }
  const gimg_bmp_g31d_code_t * code = g31d_code_for(white, run);
  if (!code) {
    return false;
  }
  g31d_put_bits(o, code->code, code->bits);
  return true;
}

GIMG_Result gimg_bmp_huffman_encode(const unsigned char * rows, uint32_t width,
    uint32_t height, size_t stride, unsigned char * out, size_t capacity,
    size_t * out_size) {
  if (!rows || !width || !height || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  gimg_bmp_g31d_out_t o = {out, capacity, 0, false};

  for (uint32_t y = 0; y < height; y++) {
    const unsigned char * row = rows + ((size_t)y * stride);
    // Every T.4 line begins with a white run, which is empty when the line
    // starts on black.  Writing that zero-length code is not optional: a
    // decoder counts colours by alternation, so leaving it out inverts the
    // line.
    bool white = true;
    uint32_t x = 0;
    while (x < width) {
      uint32_t run = 0;
      while (x + run < width) {
        bool bit = ((row[(x + run) >> 3] >> (7u - ((x + run) & 7u))) & 1u) != 0;
        if (bit == white) {  // A set bit is black, which is not white.
          break;
        }
        run++;
      }
      if (!g31d_put_run(&o, white, run)) {
        return GIMG_ERR_INTERNAL;
      }
      x += run;
      white = !white;
    }
    // An end-of-line after every line, which is what T.4 says and what makes
    // the result readable by decoders stricter than this one's.
    g31d_put_bits(&o, 1u, 12u);
  }

  if (o.overflow) {
    return GIMG_ERR_LIMIT;
  }
  *out_size = (o.bit + 7u) / 8u;
  return GIMG_OK;
}
