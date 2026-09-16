/**
 * @file
 *
 * Dequantise (scale coefficients by quant table) and 8×8 inverse DCT for
 * JPEG decode. Input: coefficient block (zigzag or row-major); output: sample
 * block. No chroma or raster; used by jpeg_entropy.c after block decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/image/macros.h>
#include "jpeg_internal.h"

void jpeg_dezigzag(const int16_t * block, int16_t * out) {
  for (int i = 0; i < 64; i++) {
    out[gimg_jpeg_zigzag[i]] = block[i];
  }
}

void jpeg_dequantise_32(
    const int16_t * block, const uint16_t * quant, int32_t * out) {
  for (int i = 0; i < 64; i++) {
    out[i] = (int32_t)block[i] * (int32_t)quant[gimg_jpeg_inv_zigzag[i]];
  }
}

#define IDCT_ISLOW_CONST_BITS 13
#define IDCT_ISLOW_ONE ((int64_t)1)
#define IDCT_ISLOW_LEFT_SHIFT(x, n) ((int64_t)((uint64_t)(x) << (n)))
#define IDCT_ISLOW_RIGHT_SHIFT(x, n) ((x) >> (n))
#define IDCT_ISLOW_DESCALE(x, n)                                               \
  IDCT_ISLOW_RIGHT_SHIFT((x) + (IDCT_ISLOW_ONE << ((n)-1)), n)
#define IDCT_ISLOW_MULTIPLY(var, c) ((int64_t)(var) * (int64_t)(c))

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

/**
 * Integer inverse DCT, the transform libjpeg calls "islow".
 *
 * T.81 A.3.3 specifies the inverse DCT mathematically and deliberately does not
 * prescribe an implementation; conformance is measured against it statistically
 * (ITU-T T.83).  Every practical decoder therefore uses a fixed-point
 * approximation, and matching the one libjpeg uses is what makes our output
 * comparable to every other decoder in the world, sample for sample.
 *
 * @param pass1_bits Fractional bits carried between the two passes.  libjpeg
 *   (jidctint.c) uses 2 for 8-bit data and 1 for 12-bit - "lose a little
 *   precision to avoid overflow" - because the first pass needs
 *   BITS_IN_JSAMPLE + PASS1_BITS + 3 bits of headroom.  Pass 2 for P=8 and 1
 *   for P=12; anything else will not agree with other decoders.
 *
 * Before this took a parameter, 12-bit frames went through a separate naive
 * float transform (a cos() per term, rounded to an integer between passes) that
 * no other decoder implements and that rounded negative values the wrong way -
 * (int)(-2.7 + 0.5) is -2, not -3.  Every 12-bit sample we produced was
 * therefore slightly wrong, and nothing external had ever checked it.
 */
void jpeg_idct_8x8_islow(const int32_t * in, int32_t * out, int pass1_bits) {
  // The intermediates are 64-bit.  This transform assumes the coefficient
  // magnitudes a conformant stream produces, and a file that does not conform
  // can drive them past int32: fuzzing reached sums like
  // -1061895276 + -1342698149 here.  The result of such a file is meaningless
  // either way, but it must not be undefined - and a decoder's arithmetic
  // should not depend on its input being well formed.  Same reasoning, and the
  // same fix, as the forward transform in jpeg_encode.c.  For conformant input
  // nothing changes: those values never came close to overflowing.
  int64_t tmp0, tmp1, tmp2, tmp3;
  int64_t tmp10, tmp11, tmp12, tmp13;
  int64_t z1, z2, z3, z4, z5;
  int64_t workspace[64];
  const int dct_bits = IDCT_ISLOW_CONST_BITS;
  const int descale_pass1 = dct_bits - pass1_bits;
  const int descale_pass2 = dct_bits + pass1_bits + 3;
  const int descale_dc_row = pass1_bits + 3;

  for (int ctr = 0; ctr < 8; ctr++) {
    const int32_t * inptr = in + ctr;
    int64_t * wsptr = workspace + ctr;

    if (inptr[8] == 0 && inptr[16] == 0 && inptr[24] == 0 && inptr[32] == 0 &&
        inptr[40] == 0 && inptr[48] == 0 && inptr[56] == 0) {
      int64_t dcval = IDCT_ISLOW_LEFT_SHIFT((int64_t)inptr[0], pass1_bits);
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
    const int64_t * wsptr = workspace + ctr * 8;
    int32_t * outptr = out + ctr * 8;

    if (wsptr[1] == 0 && wsptr[2] == 0 && wsptr[3] == 0 && wsptr[4] == 0 &&
        wsptr[5] == 0 && wsptr[6] == 0 && wsptr[7] == 0) {
      // No clamp here.  The samples this transform produces are level-shifted
      // by the caller, which clamps them to 0..2^P-1 (T.81 A.3.1) for whatever
      // P the frame declared.  Clamping to the 8-bit range inside the transform
      // - as this used to - happens to be invisible at P=8, because the caller's
      // own clamp subsumes it, but it destroys a 12-bit frame, whose samples
      // legitimately reach +-2048.
      int32_t dcval =
          (int32_t)IDCT_ISLOW_DESCALE((int64_t)wsptr[0], descale_dc_row);
      for (int c = 0; c < 8; c++) {
        outptr[c] = dcval;
      }
      continue;
    }

    // The workspace is int64_t and pass 1 can legitimately fill its range for
    // a hostile block: coefficients reach +-32767 and a quantiser value reaches
    // 65535, so the dequantised input alone is a 31-bit quantity before this
    // transform scales it further.  Narrowing to int32_t here - as this did -
    // made the sums below overflow, which is undefined behaviour rather than
    // merely a wrong pixel, and UBSan flagged it on a fuzzed round trip.  For
    // any coefficient a real image produces the values fit either way, so
    // keeping them wide changes no output; it only stops the arithmetic being
    // undefined for input that was never valid.  The caller clamps the result
    // to 0..2^P-1 (T.81 A.3.1), which is what bounds the final samples.
    z2 = wsptr[2];
    z3 = wsptr[6];
    z1 = IDCT_ISLOW_MULTIPLY(z2 + z3, idct_islow_fix_0_541196100);
    tmp2 = z1 + IDCT_ISLOW_MULTIPLY(z3, -idct_islow_fix_1_847759065);
    tmp3 = z1 + IDCT_ISLOW_MULTIPLY(z2, idct_islow_fix_0_765366865);

    tmp0 =
        IDCT_ISLOW_LEFT_SHIFT(wsptr[0] + wsptr[4], dct_bits);
    tmp1 =
        IDCT_ISLOW_LEFT_SHIFT(wsptr[0] - wsptr[4], dct_bits);

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    tmp0 = wsptr[7];
    tmp1 = wsptr[5];
    tmp2 = wsptr[3];
    tmp3 = wsptr[1];

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

    outptr[0] = (int32_t)IDCT_ISLOW_DESCALE(tmp10 + tmp3, descale_pass2);
    outptr[7] = (int32_t)IDCT_ISLOW_DESCALE(tmp10 - tmp3, descale_pass2);
    outptr[1] = (int32_t)IDCT_ISLOW_DESCALE(tmp11 + tmp2, descale_pass2);
    outptr[6] = (int32_t)IDCT_ISLOW_DESCALE(tmp11 - tmp2, descale_pass2);
    outptr[2] = (int32_t)IDCT_ISLOW_DESCALE(tmp12 + tmp1, descale_pass2);
    outptr[5] = (int32_t)IDCT_ISLOW_DESCALE(tmp12 - tmp1, descale_pass2);
    outptr[3] = (int32_t)IDCT_ISLOW_DESCALE(tmp13 + tmp0, descale_pass2);
    outptr[4] = (int32_t)IDCT_ISLOW_DESCALE(tmp13 - tmp0, descale_pass2);
  }
}

#undef IDCT_ISLOW_CONST_BITS
#undef IDCT_ISLOW_ONE
#undef IDCT_ISLOW_LEFT_SHIFT
#undef IDCT_ISLOW_RIGHT_SHIFT
#undef IDCT_ISLOW_DESCALE
#undef IDCT_ISLOW_MULTIPLY
