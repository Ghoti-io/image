/**
 * @file
 *
 * Dequantise (scale coefficients by quant table) and 8×8 inverse DCT for
 * JPEG decode. Input: coefficient block (zigzag or row-major); output: sample
 * block. No chroma or raster; used by jpeg_entropy.c after block decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/image/macros.h>
#include "jpeg_internal.h"

void jpeg_dezigzag(const int16_t * block, int16_t * out) {
  for (int i = 0; i < 64; i++) {
    out[gimg_jpeg_zigzag[i]] = block[i];
  }
}

void jpeg_dequantise(
    const int16_t * block, const uint16_t * quant, int16_t * out) {
  for (int i = 0; i < 64; i++) {
    out[i] = (int16_t)((int)block[i] * (int)quant[gimg_jpeg_inv_zigzag[i]]);
  }
}

void jpeg_dequantise_32(
    const int16_t * block, const uint16_t * quant, int32_t * out) {
  for (int i = 0; i < 64; i++) {
    out[i] = (int32_t)block[i] * (int32_t)quant[gimg_jpeg_inv_zigzag[i]];
  }
}

static void jpeg_idct_1d(const int16_t * in, int16_t * out) {
  static const int scale = 256;
  for (int x = 0; x < 8; x++) {
    double sum = 0.0;
    for (int u = 0; u < 8; u++) {
      int32_t c = (u == 0) ? 181 : 256;
      double angle = (2 * x + 1) * u * 3.14159265358979323846 / 16.0;
      sum += (double)in[u] * (double)c * cos(angle);
    }
    out[x] = (int16_t)(int)(sum / (double)scale + 0.5);
  }
}

static void jpeg_idct_1d_32(const int32_t * in, int32_t * out, int scale) {
  for (int x = 0; x < 8; x++) {
    double sum = 0.0;
    for (int u = 0; u < 8; u++) {
      int32_t c = (u == 0) ? 181 : 256;
      double angle = (2 * x + 1) * u * 3.14159265358979323846 / 16.0;
      sum += (double)in[u] * (double)c * cos(angle);
    }
    out[x] = (int32_t)(int)(sum / (double)scale + 0.5);
  }
}

void jpeg_idct_8x8(const int16_t * in, int16_t * out) {
  int16_t row[8];
  int16_t tmp[64];
  for (int y = 0; y < 8; y++) {
    jpeg_idct_1d(in + y * 8, row);
    for (int x = 0; x < 8; x++) {
      tmp[x * 8 + y] = row[x];
    }
  }
  for (int x = 0; x < 8; x++) {
    jpeg_idct_1d(tmp + x * 8, row);
    for (int y = 0; y < 8; y++) {
      out[y * 8 + x] = row[y];
    }
  }
}

void jpeg_idct_8x8_32(const int32_t * in, int32_t * out, int scale) {
  int32_t row[8];
  int32_t tmp[64];
  for (int y = 0; y < 8; y++) {
    jpeg_idct_1d_32(in + y * 8, row, scale);
    for (int x = 0; x < 8; x++) {
      tmp[x * 8 + y] = row[x];
    }
  }
  for (int x = 0; x < 8; x++) {
    jpeg_idct_1d_32(tmp + x * 8, row, scale);
    for (int y = 0; y < 8; y++) {
      out[y * 8 + x] = row[y];
    }
  }
}

#define IDCT_ISLOW_CONST_BITS 13
#define IDCT_ISLOW_PASS1_BITS 2
#define IDCT_ISLOW_ONE ((int32_t)1)
#define IDCT_ISLOW_LEFT_SHIFT(x, n) GIMG_JPEG_LSHIFT(x, n)
#define IDCT_ISLOW_RIGHT_SHIFT(x, n) ((x) >> (n))
#define IDCT_ISLOW_DESCALE(x, n)                                               \
  IDCT_ISLOW_RIGHT_SHIFT((x) + (IDCT_ISLOW_ONE << ((n)-1)), n)
#define IDCT_ISLOW_MULTIPLY(var, c) ((int32_t)(var) * (int32_t)(c))

static const int32_t idct_islow_fix_0_298631336 = 2446;
static const int32_t idct_islow_fix_0_390180644 = 3196;
static const int32_t idct_islow_fix_0_541196100 = 4433;
static const int32_t idct_islow_fix_0_765366865 = 6270;
static const int32_t idct_islow_fix_0_899976223 = 7373;
static const int32_t idct_islow_fix_1_175875602 = 9633;
static const int32_t idct_islow_fix_1_501321110 = 12299;
static const int32_t idct_islow_fix_1_847759065 = 15137;
static const int32_t idct_islow_fix_1_961570560 = 16069;
static const int32_t idct_islow_fix_2_053119869 = 16819;
static const int32_t idct_islow_fix_2_562915447 = 20995;
static const int32_t idct_islow_fix_3_072711026 = 25172;

void jpeg_idct_8x8_islow(const int16_t * in, int16_t * out) {
  int32_t tmp0, tmp1, tmp2, tmp3;
  int32_t tmp10, tmp11, tmp12, tmp13;
  int32_t z1, z2, z3, z4, z5;
  int workspace[64];
  const int dct_bits = IDCT_ISLOW_CONST_BITS;
  const int pass1_bits = IDCT_ISLOW_PASS1_BITS;
  const int descale_pass1 = dct_bits - pass1_bits;
  const int descale_pass2 = dct_bits + pass1_bits + 3;
  const int descale_dc_row = pass1_bits + 3;

  for (int ctr = 0; ctr < 8; ctr++) {
    const int16_t * inptr = in + ctr;
    int * wsptr = workspace + ctr;

    if (inptr[8] == 0 && inptr[16] == 0 && inptr[24] == 0 && inptr[32] == 0 &&
        inptr[40] == 0 && inptr[48] == 0 && inptr[56] == 0) {
      int32_t dcval = IDCT_ISLOW_LEFT_SHIFT((int32_t)inptr[0], pass1_bits);
      for (int r = 0; r < 8; r++) {
        wsptr[r * 8] = (int)dcval;
      }
      continue;
    }

    z2 = (int32_t)inptr[16];
    z3 = (int32_t)inptr[48];
    z1 = IDCT_ISLOW_MULTIPLY(z2 + z3, idct_islow_fix_0_541196100);
    tmp2 = z1 + IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_847759065);
    tmp3 = z1 + IDCT_ISLOW_MULTIPLY(z2, idct_islow_fix_0_765366865);

    z2 = (int32_t)inptr[0];
    z3 = (int32_t)inptr[32];
    tmp0 = IDCT_ISLOW_LEFT_SHIFT(z2 + z3, dct_bits);
    tmp1 = IDCT_ISLOW_LEFT_SHIFT(z2 - z3, dct_bits);

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = (int32_t)inptr[56];
    tmp1 = (int32_t)inptr[40];
    tmp2 = (int32_t)inptr[24];
    tmp3 = (int32_t)inptr[8];

    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = IDCT_ISLOW_MULTIPLY(z3 + z4, idct_islow_fix_1_175875602);

    tmp0 = IDCT_ISLOW_MULTIPLY(tmp0, idct_islow_fix_0_298631336);
    tmp1 = IDCT_ISLOW_MULTIPLY(tmp1, idct_islow_fix_2_053119869);
    tmp2 = IDCT_ISLOW_MULTIPLY(tmp2, idct_islow_fix_3_072711026);
    tmp3 = IDCT_ISLOW_MULTIPLY(tmp3, idct_islow_fix_1_501321110);
    z1 = IDCT_ISLOW_MULTIPLY(z1, -idct_islow_fix_0_899976223);
    z2 = IDCT_ISLOW_MULTIPLY(z2, -idct_islow_fix_2_562915447);
    z3 = IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_961570560);
    z4 = IDCT_ISLOW_MULTIPLY(z4, -idct_islow_fix_0_390180644);

    z3 += z5;
    z4 += z5;

    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    wsptr[0] = (int)IDCT_ISLOW_DESCALE(tmp10 + tmp3, descale_pass1);
    wsptr[56] = (int)IDCT_ISLOW_DESCALE(tmp10 - tmp3, descale_pass1);
    wsptr[8] = (int)IDCT_ISLOW_DESCALE(tmp11 + tmp2, descale_pass1);
    wsptr[48] = (int)IDCT_ISLOW_DESCALE(tmp11 - tmp2, descale_pass1);
    wsptr[16] = (int)IDCT_ISLOW_DESCALE(tmp12 + tmp1, descale_pass1);
    wsptr[40] = (int)IDCT_ISLOW_DESCALE(tmp12 - tmp1, descale_pass1);
    wsptr[24] = (int)IDCT_ISLOW_DESCALE(tmp13 + tmp0, descale_pass1);
    wsptr[32] = (int)IDCT_ISLOW_DESCALE(tmp13 - tmp0, descale_pass1);
  }

  for (int ctr = 0; ctr < 8; ctr++) {
    const int * wsptr = workspace + ctr * 8;
    int16_t * outptr = out + ctr * 8;

    if (wsptr[1] == 0 && wsptr[2] == 0 && wsptr[3] == 0 && wsptr[4] == 0 &&
        wsptr[5] == 0 && wsptr[6] == 0 && wsptr[7] == 0) {
      int32_t v = IDCT_ISLOW_DESCALE((int32_t)wsptr[0], descale_dc_row);
      if (v < -128)
        v = -128;
      if (v > 127)
        v = 127;
      int16_t dcval = (int16_t)v;
      for (int c = 0; c < 8; c++) {
        outptr[c] = dcval;
      }
      continue;
    }

    z2 = (int32_t)wsptr[2];
    z3 = (int32_t)wsptr[6];
    z1 = IDCT_ISLOW_MULTIPLY(z2 + z3, idct_islow_fix_0_541196100);
    tmp2 = z1 + IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_847759065);
    tmp3 = z1 + IDCT_ISLOW_MULTIPLY(z2, idct_islow_fix_0_765366865);

    tmp0 =
        IDCT_ISLOW_LEFT_SHIFT((int32_t)wsptr[0] + (int32_t)wsptr[4], dct_bits);
    tmp1 =
        IDCT_ISLOW_LEFT_SHIFT((int32_t)wsptr[0] - (int32_t)wsptr[4], dct_bits);

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = (int32_t)wsptr[7];
    tmp1 = (int32_t)wsptr[5];
    tmp2 = (int32_t)wsptr[3];
    tmp3 = (int32_t)wsptr[1];

    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    z4 = tmp1 + tmp3;
    z5 = IDCT_ISLOW_MULTIPLY(z3 + z4, idct_islow_fix_1_175875602);

    tmp0 = IDCT_ISLOW_MULTIPLY(tmp0, idct_islow_fix_0_298631336);
    tmp1 = IDCT_ISLOW_MULTIPLY(tmp1, idct_islow_fix_2_053119869);
    tmp2 = IDCT_ISLOW_MULTIPLY(tmp2, idct_islow_fix_3_072711026);
    tmp3 = IDCT_ISLOW_MULTIPLY(tmp3, idct_islow_fix_1_501321110);
    z1 = IDCT_ISLOW_MULTIPLY(z1, -idct_islow_fix_0_899976223);
    z2 = IDCT_ISLOW_MULTIPLY(z2, -idct_islow_fix_2_562915447);
    z3 = IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_961570560);
    z4 = IDCT_ISLOW_MULTIPLY(z4, -idct_islow_fix_0_390180644);

    z3 += z5;
    z4 += z5;

    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    outptr[0] = (int16_t)IDCT_ISLOW_DESCALE(tmp10 + tmp3, descale_pass2);
    outptr[7] = (int16_t)IDCT_ISLOW_DESCALE(tmp10 - tmp3, descale_pass2);
    outptr[1] = (int16_t)IDCT_ISLOW_DESCALE(tmp11 + tmp2, descale_pass2);
    outptr[6] = (int16_t)IDCT_ISLOW_DESCALE(tmp11 - tmp2, descale_pass2);
    outptr[2] = (int16_t)IDCT_ISLOW_DESCALE(tmp12 + tmp1, descale_pass2);
    outptr[5] = (int16_t)IDCT_ISLOW_DESCALE(tmp12 - tmp1, descale_pass2);
    outptr[3] = (int16_t)IDCT_ISLOW_DESCALE(tmp13 + tmp0, descale_pass2);
    outptr[4] = (int16_t)IDCT_ISLOW_DESCALE(tmp13 - tmp0, descale_pass2);
  }
}

#undef IDCT_ISLOW_CONST_BITS
#undef IDCT_ISLOW_PASS1_BITS
#undef IDCT_ISLOW_ONE
#undef IDCT_ISLOW_LEFT_SHIFT
#undef IDCT_ISLOW_RIGHT_SHIFT
#undef IDCT_ISLOW_DESCALE
#undef IDCT_ISLOW_MULTIPLY
