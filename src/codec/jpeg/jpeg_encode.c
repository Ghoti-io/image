/**
 * JPEG encoder: FDCT, quantize, fill coef buffer, baseline scan encode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "jpeg_debug_internal.h"
#include "jpeg_huffman_tables_internal.h"
#include "jpeg_internal.h"

#define DCTSIZE 8
#define DCTSIZE2 64

// 8×8 FDCT per T.81 Annex F (integer Loeffler-style; CONST_BITS=13, PASS1_BITS=2).
#define CONST_BITS 13
#define PASS1_BITS 2
#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

// The DCT intermediates are computed in 64 bits.  At 12-bit sample precision an
// intermediate reaches ~2.6e5 and the fixed-point constants are up to 25172, so
// the product exceeds int32 (UBSan: "signed integer overflow: -259968 * 9633").
// libjpeg solves the same problem by widening DCTELEM to int for its 12-bit
// build; 64 bits removes the question entirely and costs nothing on a 64-bit
// target.  For 8-bit input the results are unchanged: nothing overflowed there.
#define DESCALE(x, n) (((int64_t)(x) + ((int64_t)1 << ((n)-1))) >> (n))
#define MULTIPLY(var, constval) ((int64_t)(var) * (int64_t)(constval))

// Bit position of highest set bit (1-based); 0 if x==0. Used for reciprocal quant.
static int flss_u32(uint32_t x) {
  if (x == 0)
    return 0;
  int n = 0;
  while (x != 0) {
    n++;
    x >>= 1;
  }
  return n;
}

// Compute reciprocal quantizer parameters for one divisor (libjpeg 8-bit style).
// divisor = 8 * quant_table[i]. Stores recip, corr, unused, shift into tbl[0..3].
static void compute_reciprocal(uint32_t divisor, int16_t * tbl) {
  if (divisor <= 1) {
    tbl[0] = 1;
    tbl[1] = 0;
    tbl[2] = 1;
    tbl[3] = (int16_t)(-(int)(sizeof(int16_t) * 8));
    return;
  }
  int b = flss_u32(divisor) - 1;
  int r = (int)(sizeof(int16_t) * 8) + b;
  uint32_t fq = ((uint32_t)1 << r) / divisor;
  uint32_t fr = ((uint32_t)1 << r) % divisor;
  uint32_t c = divisor / 2u;
  if (fr == 0) {
    fq >>= 1;
    r--;
  }
  else if (fr <= (divisor / 2u)) {
    c++;
  }
  else {
    fq++;
  }
  tbl[0] = (int16_t)(uint16_t)fq;
  tbl[1] = (int16_t)(uint16_t)c;
  tbl[2] = 1;
  tbl[3] = (int16_t)(r - (int)(sizeof(int16_t) * 8));
}

static void jpeg_fdct_islow(int32_t * data) {
  int64_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
  int64_t tmp10, tmp11, tmp12, tmp13;
  int64_t z1, z2, z3, z4, z5;
  int32_t * dataptr;
  int ctr;

  dataptr = data;
  for (ctr = DCTSIZE - 1; ctr >= 0; ctr--) {
    tmp0 = dataptr[0] + dataptr[7];
    tmp7 = dataptr[0] - dataptr[7];
    tmp1 = dataptr[1] + dataptr[6];
    tmp6 = dataptr[1] - dataptr[6];
    tmp2 = dataptr[2] + dataptr[5];
    tmp5 = dataptr[2] - dataptr[5];
    tmp3 = dataptr[3] + dataptr[4];
    tmp4 = dataptr[3] - dataptr[4];

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    dataptr[0] = (int32_t)GIMG_JPEG_LSHIFT(tmp10 + tmp11, PASS1_BITS);
    dataptr[4] = (int32_t)GIMG_JPEG_LSHIFT(tmp10 - tmp11, PASS1_BITS);

    z1 = MULTIPLY(tmp12 + tmp13, FIX_0_541196100);
    dataptr[2] = (int32_t)DESCALE(
        z1 + MULTIPLY(tmp13, FIX_0_765366865), CONST_BITS - PASS1_BITS);
    dataptr[6] = (int32_t)DESCALE(
        z1 + MULTIPLY(tmp12, -FIX_1_847759065), CONST_BITS - PASS1_BITS);

    z1 = tmp4 + tmp7;
    z2 = tmp5 + tmp6;
    z3 = tmp4 + tmp6;
    z4 = tmp5 + tmp7;
    z5 = MULTIPLY(z3 + z4, FIX_1_175875602);

    tmp4 = MULTIPLY(tmp4, FIX_0_298631336);
    tmp5 = MULTIPLY(tmp5, FIX_2_053119869);
    tmp6 = MULTIPLY(tmp6, FIX_3_072711026);
    tmp7 = MULTIPLY(tmp7, FIX_1_501321110);
    z1 = MULTIPLY(z1, -FIX_0_899976223);
    z2 = MULTIPLY(z2, -FIX_2_562915447);
    z3 = MULTIPLY(z3, -FIX_1_961570560);
    z4 = MULTIPLY(z4, -FIX_0_390180644);

    z3 += z5;
    z4 += z5;

    dataptr[7] = (int32_t)DESCALE(tmp4 + z1 + z3, CONST_BITS - PASS1_BITS);
    dataptr[5] = (int32_t)DESCALE(tmp5 + z2 + z4, CONST_BITS - PASS1_BITS);
    dataptr[3] = (int32_t)DESCALE(tmp6 + z2 + z3, CONST_BITS - PASS1_BITS);
    dataptr[1] = (int32_t)DESCALE(tmp7 + z1 + z4, CONST_BITS - PASS1_BITS);

    dataptr += DCTSIZE;
  }

  dataptr = data;
  for (ctr = DCTSIZE - 1; ctr >= 0; ctr--) {
    tmp0 = dataptr[DCTSIZE * 0] + dataptr[DCTSIZE * 7];
    tmp7 = dataptr[DCTSIZE * 0] - dataptr[DCTSIZE * 7];
    tmp1 = dataptr[DCTSIZE * 1] + dataptr[DCTSIZE * 6];
    tmp6 = dataptr[DCTSIZE * 1] - dataptr[DCTSIZE * 6];
    tmp2 = dataptr[DCTSIZE * 2] + dataptr[DCTSIZE * 5];
    tmp5 = dataptr[DCTSIZE * 2] - dataptr[DCTSIZE * 5];
    tmp3 = dataptr[DCTSIZE * 3] + dataptr[DCTSIZE * 4];
    tmp4 = dataptr[DCTSIZE * 3] - dataptr[DCTSIZE * 4];

    tmp10 = tmp0 + tmp3;
    tmp13 = tmp0 - tmp3;
    tmp11 = tmp1 + tmp2;
    tmp12 = tmp1 - tmp2;

    dataptr[DCTSIZE * 0] = (int32_t)DESCALE(tmp10 + tmp11, PASS1_BITS);
    dataptr[DCTSIZE * 4] = (int32_t)DESCALE(tmp10 - tmp11, PASS1_BITS);

    z1 = MULTIPLY(tmp12 + tmp13, FIX_0_541196100);
    dataptr[DCTSIZE * 2] = (int32_t)DESCALE(
        z1 + MULTIPLY(tmp13, FIX_0_765366865), CONST_BITS + PASS1_BITS);
    dataptr[DCTSIZE * 6] = (int32_t)DESCALE(
        z1 + MULTIPLY(tmp12, -FIX_1_847759065), CONST_BITS + PASS1_BITS);

    z1 = tmp4 + tmp7;
    z2 = tmp5 + tmp6;
    z3 = tmp4 + tmp6;
    z4 = tmp5 + tmp7;
    z5 = MULTIPLY(z3 + z4, FIX_1_175875602);

    tmp4 = MULTIPLY(tmp4, FIX_0_298631336);
    tmp5 = MULTIPLY(tmp5, FIX_2_053119869);
    tmp6 = MULTIPLY(tmp6, FIX_3_072711026);
    tmp7 = MULTIPLY(tmp7, FIX_1_501321110);
    z1 = MULTIPLY(z1, -FIX_0_899976223);
    z2 = MULTIPLY(z2, -FIX_2_562915447);
    z3 = MULTIPLY(z3, -FIX_1_961570560);
    z4 = MULTIPLY(z4, -FIX_0_390180644);

    z3 += z5;
    z4 += z5;

    dataptr[DCTSIZE * 7] =
        (int32_t)DESCALE(tmp4 + z1 + z3, CONST_BITS + PASS1_BITS);
    dataptr[DCTSIZE * 5] =
        (int32_t)DESCALE(tmp5 + z2 + z4, CONST_BITS + PASS1_BITS);
    dataptr[DCTSIZE * 3] =
        (int32_t)DESCALE(tmp6 + z2 + z3, CONST_BITS + PASS1_BITS);
    dataptr[DCTSIZE * 1] =
        (int32_t)DESCALE(tmp7 + z1 + z4, CONST_BITS + PASS1_BITS);

    dataptr++;
  }
}

// Quantize DCT coefficients per T.81 Annex F (round to integer).
static void jpeg_quantize_block(
    const int32_t * block, const uint16_t * quant, int16_t * out) {
  for (int z = 0; z < 64; z++) {
    int nat = (int)gimg_jpeg_zigzag[z];
    int32_t val = (int32_t)block[nat];
    uint16_t q = quant[nat];
    uint16_t q8 = q << 3;
    int32_t result;
    if (q8 == 0) {
      result = 0;
    }
    else {
      if (val < 0) {
        val = -val;
        result = (int32_t)((val + (q << 2)) / (int32_t)q8);
        result = -result;
      }
      else {
        result = (int32_t)((val + (q << 2)) / (int32_t)q8);
      }
    }
    if (result < -32768)
      result = -32768;
    if (result > 32767)
      result = 32767;
    out[z] = (int16_t)result;
  }
}

// Quantize for 16-bit DQT: quant entries are uint16_t; use 32-bit divisor (q*8).
// T.81 Annex F rounding. Table in natural order (same as 8-bit path).
static void jpeg_quantize_block_16bit(
    const int32_t * block, const uint16_t * quant, int16_t * out) {
  for (int z = 0; z < 64; z++) {
    int nat = (int)gimg_jpeg_zigzag[z];
    int32_t val = (int32_t)block[nat];
    uint32_t q = (uint32_t)quant[nat];
    uint32_t q8 = q << 3;
    int32_t result;
    if (q8 == 0) {
      result = 0;
    }
    else {
      if (val < 0) {
        val = -val;
        result = (int32_t)((val + (int32_t)(q << 2)) / (int32_t)q8);
        result = -result;
      }
      else {
        result = (int32_t)((val + (int32_t)(q << 2)) / (int32_t)q8);
      }
    }
    if (result < -32768)
      result = -32768;
    if (result > 32767)
      result = 32767;
    out[z] = (int16_t)result;
  }
}

#define RECIP_STRIDE 4 // recip, corr, scale, shift per coefficient

// Quantize using reciprocal (libjpeg 8-bit path); dtbl indexed by natural order.
// Use 64-bit product so (temp+corr)*recip does not overflow before shifting.
static void jpeg_quantize_block_recip(
    const int32_t * block, const int16_t * dtbl, int16_t * out) {
  const int sh = (int)(sizeof(int16_t) * 8); // 16
  for (int z = 0; z < 64; z++) {
    int nat = (int)gimg_jpeg_zigzag[z];
    int32_t temp = (int32_t)block[nat];
    int32_t recip = (int32_t)dtbl[nat * RECIP_STRIDE + 0];
    int32_t corr = (int32_t)dtbl[nat * RECIP_STRIDE + 1];
    int shift = (int)dtbl[nat * RECIP_STRIDE + 3];
    uint64_t sum = (uint64_t)(temp >= 0 ? (uint32_t)(temp + corr)
                                        : (uint32_t)(-temp + corr));
    uint64_t product = sum * (uint64_t)(uint16_t)recip;
    int32_t result = (int32_t)(product >> (shift + sh));
    if (temp < 0)
      result = -result;
    if (result < -32768)
      result = -32768;
    if (result > 32767)
      result = 32767;
    out[z] = (int16_t)result;
  }
}

// Component dimensions in 8×8 blocks (T.81 Annex A: X_i = (X*H_i+H_max*8-1)/(H_max*8)
// in blocks, so width_blocks = (width*h_samp+h_max*8-1)/(h_max*8).
static void jpeg_comp_blocks(uint32_t width, uint32_t height, uint8_t h_samp,
    uint8_t v_samp, uint8_t h_max, uint8_t v_max, uint32_t * out_w,
    uint32_t * out_h) {
  uint32_t denom_w = (uint32_t)h_max * 8u;
  uint32_t denom_h = (uint32_t)v_max * 8u;
  *out_w = (width * (uint32_t)h_samp + denom_w - 1u) / denom_w;
  *out_h = (height * (uint32_t)v_samp + denom_h - 1u) / denom_h;
}

// Component size in samples per line and per column (T.81 Annex A:
// X_i = ceil(X*H_i/H_max), Y_i = ceil(Y*V_i/V_max). Used when a component
// has fewer than 8 samples in a dimension so we replicate into the 8×8 block.
static void jpeg_comp_pixels(uint32_t width, uint32_t height, uint8_t h_samp,
    uint8_t v_samp, uint8_t h_max, uint8_t v_max, uint32_t * out_pix_w,
    uint32_t * out_pix_h) {
  *out_pix_w =
      (width * (uint32_t)h_samp + (uint32_t)h_max - 1u) / (uint32_t)h_max;
  *out_pix_h =
      (height * (uint32_t)v_samp + (uint32_t)v_max - 1u) / (uint32_t)v_max;
}

GIMG_Result gimg_jpeg_progressive_fill_coef_buffer(uint32_t width,
    uint32_t height, int num_components, const unsigned char * comp0,
    const unsigned char * comp1, const unsigned char * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, unsigned fdct_method, unsigned quant_method,
    int16_t * coef_buffer, size_t * out_total_blocks) {
  (void)fdct_method;
  static const uint8_t default_samp[3] = {1, 1, 1};
  if (!h_samp)
    h_samp = default_samp;
  if (!v_samp)
    v_samp = default_samp;
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max)
      h_max = h_samp[1];
    if (h_samp[2] > h_max)
      h_max = h_samp[2];
    if (v_samp[1] > v_max)
      v_max = v_samp[1];
    if (v_samp[2] > v_max)
      v_max = v_samp[2];
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  size_t total_blocks = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gcu_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
    return GIMG_ERR_LIMIT;
  }
  *out_total_blocks = total_blocks;

  // Reciprocal quantizer tables (libjpeg 8-bit style) when quant_method is RECIP.
  static int16_t recip_luma[64 * RECIP_STRIDE];
  static int16_t recip_chroma[64 * RECIP_STRIDE];
  if (quant_method == GIMG_JPEG_QUANT_RECIP) {
    for (int i = 0; i < 64; i++) {
      uint32_t div_l = (uint32_t)(quant_luma[i] << 3);
      uint32_t div_c = (uint32_t)(quant_chroma[i] << 3);
      if (div_l <= 1)
        div_l = 1;
      if (div_c <= 1)
        div_c = 1;
      compute_reciprocal(div_l, &recip_luma[i * RECIP_STRIDE]);
      compute_reciprocal(div_c, &recip_chroma[i * RECIP_STRIDE]);
    }
  }

  const unsigned char * comps[3] = {comp0, comp1, comp2};
  size_t strides[3] = {stride0, stride1, stride2};
  uint32_t comp_w[3], comp_h[3];
  uint32_t comp_pix_w[3], comp_pix_h[3];
  for (int c = 0; c < num_components; c++) {
    jpeg_comp_blocks(width, height, h_samp[c], v_samp[c], h_max, v_max,
        &comp_w[c], &comp_h[c]);
    jpeg_comp_pixels(width, height, h_samp[c], v_samp[c], h_max, v_max,
        &comp_pix_w[c], &comp_pix_h[c]);
  }

  int16_t * out = coef_buffer;
  int32_t block[64];
  // Per-component last DC for dummy blocks (T.81 Annex A: padding blocks use
  // zero AC and DC = previous block DC so diff = 0).
  int16_t last_dc[3] = {0, 0, 0};

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      for (int c = 0; c < num_components; c++) {
        const uint16_t * quant = (c == 0) ? quant_luma : quant_chroma;
        const unsigned char * comp = comps[c];
        size_t stride = strides[c];
        uint32_t cw = comp_w[c];
        uint32_t ch = comp_h[c];
        uint32_t pix_w = comp_pix_w[c];
        uint32_t pix_h = comp_pix_h[c];
        // T.81 Annex A: sample within component dimensions; clamp to pix_w so we do not
        // read past the buffer when stride equals image width (e.g. 1×1).
        for (uint32_t by = 0; by < (uint32_t)v_samp[c]; by++) {
          for (uint32_t bx = 0; bx < (uint32_t)h_samp[c]; bx++) {
            uint32_t blk_x = mcu_x * (uint32_t)h_samp[c] + bx;
            uint32_t blk_y = mcu_y * (uint32_t)v_samp[c] + by;
            if (blk_x >= cw || blk_y >= ch) {
              // T.81 Annex A: padding/dummy block — zero AC, DC = previous DC.
              out[0] = last_dc[c];
              for (int i = 1; i < 64; i++)
                out[i] = 0;
              out += 64;
              continue;
            }
            uint32_t px = blk_x * 8;
            uint32_t py = blk_y * 8;
            for (int row = 0; row < 8; row++) {
              uint32_t y_src = (py + (uint32_t)row) < pix_h
                  ? (py + (uint32_t)row)
                  : (pix_h - 1u);
              size_t row_off = (size_t)y_src * stride;
              for (int col = 0; col < 8; col++) {
                uint32_t x_src = px + (uint32_t)col;
                if (x_src >= pix_w)
                  x_src = pix_w - 1u;
                unsigned char s = comp[row_off + (size_t)x_src];
                block[row * 8 + col] = (int16_t)((int)s - 128);
              }
            }
            // Optional debug: dump Cb (component 1) presamples for comparison with reference.
#if GIMG_JPEG_DUMP_FIRST_MCU_COEF
            if (c == 1 && mcu_x == 0 && mcu_y == 0 && by == 0 && bx == 0) {
              const char * dump_dir =
                  getenv("GIMG_JPEG_DUMP_FIRST_MCU_COEF");
              if (dump_dir && dump_dir[0] != '\0') {
                char path[1024];
                int n = snprintf(
                    path, sizeof(path), "%s/cb_presamples.bin", dump_dir);
                if (n > 0 && (size_t)n < sizeof(path)) {
                  FILE * f = fopen(path, "wb");
                  if (f) {
                    (void)fwrite(block, sizeof(block[0]), 64, f);
                    (void)fclose(f);
                  }
                }
              }
            }
#endif
            jpeg_fdct_islow(block);
            // Optional debug: dump Cb after FDCT (before quant) for comparison.
#if GIMG_JPEG_DUMP_FIRST_MCU_COEF
            if (c == 1 && mcu_x == 0 && mcu_y == 0 && by == 0 && bx == 0) {
              const char * dump_dir =
                  getenv("GIMG_JPEG_DUMP_FIRST_MCU_COEF");
              if (dump_dir && dump_dir[0] != '\0') {
                char path[1024];
                int n = snprintf(
                    path, sizeof(path), "%s/cb_after_fdct.bin", dump_dir);
                if (n > 0 && (size_t)n < sizeof(path)) {
                  FILE * f = fopen(path, "wb");
                  if (f) {
                    (void)fwrite(block, sizeof(block[0]), 64, f);
                    (void)fclose(f);
                  }
                }
              }
            }
#endif
            if (quant_method == GIMG_JPEG_QUANT_RECIP) {
              const int16_t * rtbl = (c == 0) ? recip_luma : recip_chroma;
              jpeg_quantize_block_recip(block, rtbl, out);
            }
            else {
              jpeg_quantize_block(block, quant, out);
            }
            // Optional: dump first MCU Cb/Cr quantized blocks (zigzag) for comparison with libjpeg.
#if GIMG_JPEG_DUMP_FIRST_MCU_COEF
            if (mcu_x == 0 && mcu_y == 0 && by == 0 && bx == 0) {
              const char * dump_dir =
                  getenv("GIMG_JPEG_DUMP_FIRST_MCU_COEF");
              if (dump_dir && dump_dir[0] != '\0') {
                char path[1024];
                const char * name = (c == 1) ? "cb_coef_ours.bin"
                    : (c == 2)               ? "cr_coef_ours.bin"
                                             : NULL;
                if (name) {
                  int n = snprintf(path, sizeof(path), "%s/%s", dump_dir, name);
                  if (n > 0 && (size_t)n < sizeof(path)) {
                    FILE * f = fopen(path, "wb");
                    if (f) {
                      (void)fwrite(out, sizeof(out[0]), 64, f);
                      (void)fclose(f);
                    }
                  }
                }
              }
            }
#endif
            last_dc[c] = out[0];
            out += 64;
          }
        }
      }
    }
  }
  return GIMG_OK;
}

typedef struct {
  unsigned int code[256];
  int len[256];
} jpeg_derived_tbl;

static void build_derived_tbl(const unsigned char * bits,
    const unsigned char * vals, int nvals, jpeg_derived_tbl * tbl) {
  char huffsize[257];
  unsigned int huffcode[257];
  unsigned int code = 0;
  int p = 0;
  for (int l = 1; l <= 16; l++) {
    int n = (int)bits[l - 1];
    for (int i = 0; i < n; i++) {
      huffsize[p++] = (char)l;
    }
  }
  int lastp = p;
  if (lastp == 0) {
    for (int i = 0; i < 256; i++) {
      tbl->code[i] = 0;
      tbl->len[i] = 0;
    }
    return;
  }
  int si = (int)huffsize[0];
  p = 0;
  while (p < lastp) {
    while (p < lastp && (int)huffsize[p] == si) {
      huffcode[p++] = code++;
    }
    if (si < 16) {
      code <<= 1;
      si++;
    }
  }
  for (int i = 0; i < 256; i++) {
    tbl->code[i] = 0;
    tbl->len[i] = 0;
  }
  for (p = 0; p < lastp && p < nvals; p++) {
    int sym = (int)vals[p];
    if (sym >= 0 && sym < 256) {
      tbl->code[sym] = huffcode[p];
      tbl->len[sym] = (int)huffsize[p];
    }
  }
}

typedef struct {
  unsigned char * buf;
  size_t cap;
  size_t len;
  uint64_t bitbuf;
  int nbits;
} jpeg_bit_writer;

static int bit_writer_ensure(
    jpeg_bit_writer * w, const GIMG_Allocator * alloc, size_t extra) {
  if (w->len + extra <= w->cap)
    return 1;
  size_t new_cap = w->cap ? w->cap * 2 : 4096;
  if (new_cap < w->len + extra)
    new_cap = w->len + extra;
  unsigned char * p = (unsigned char *)gimg_realloc(alloc, w->buf, new_cap);
  if (!p)
    return 0;
  w->buf = p;
  w->cap = new_cap;
  return 1;
}

static void bit_writer_put_byte(
    jpeg_bit_writer * w, const GIMG_Allocator * alloc, unsigned char b) {
  if (!bit_writer_ensure(w, alloc, 2))
    return;
  if (b == 0xFF && w->buf) {
    w->buf[w->len++] = 0xFF;
    w->buf[w->len++] = 0x00;
  }
  else if (w->buf) {
    w->buf[w->len++] = b;
  }
}

static void bit_writer_put_bits(jpeg_bit_writer * w,
    const GIMG_Allocator * alloc, unsigned int code, int nbits) {
  w->bitbuf = (w->bitbuf << nbits) | (code & ((1u << nbits) - 1));
  w->nbits += nbits;
  while (w->nbits >= 8) {
    w->nbits -= 8;
    unsigned char b = (unsigned char)(w->bitbuf >> w->nbits);
    bit_writer_put_byte(w, alloc, b);
    w->bitbuf &= (1ULL << w->nbits) - 1;
  }
}

// T.81 B.2.2 / B.2.4: the value of padding bits in the final incomplete byte
// of an entropy-coded segment is unspecified (encoder choice). We fill the
// partial byte with ones; decoders must not interpret padding as data.
static void bit_writer_flush(
    jpeg_bit_writer * w, const GIMG_Allocator * alloc) {
  while (w->nbits > 0) {
    int n = w->nbits >= 8 ? 8 : w->nbits;
    w->nbits -= n;
    unsigned char b = (unsigned char)((w->bitbuf << (8 - n)) | (0xFFu >> n));
    bit_writer_put_byte(w, alloc, b);
    w->bitbuf &= (1ULL << w->nbits) - 1;
  }
}

static int jpeg_nbits(int val) {
  if (val < 0)
    val = -val;
  if (val == 0)
    return 0;
  int n = 0;
  while (val) {
    n++;
    val >>= 1;
  }
  return n;
}

GIMG_Result gimg_jpeg_encode_baseline_scan_from_coef_buffer(uint32_t width,
    uint32_t height, int num_components, const int16_t * coef_buffer,
    size_t total_blocks, const uint8_t * h_samp, const uint8_t * v_samp,
    const GIMG_Allocator * alloc, uint16_t restart_interval,
    unsigned char ** out_scan_data, size_t * out_scan_size) {
  if (!alloc || !out_scan_data || !out_scan_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_scan_data = NULL;
  *out_scan_size = 0;

  static const uint8_t default_samp[3] = {1, 1, 1};
  if (!h_samp)
    h_samp = default_samp;
  if (!v_samp)
    v_samp = default_samp;
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max)
      h_max = h_samp[1];
    if (h_samp[2] > h_max)
      h_max = h_samp[2];
    if (v_samp[1] > v_max)
      v_max = v_samp[1];
    if (v_samp[2] > v_max)
      v_max = v_samp[2];
  }
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  uint32_t mcu_per_row =
      (width + (uint32_t)(8 * h_max) - 1) / (uint32_t)(8 * h_max);
  uint32_t mcu_per_col =
      (height + (uint32_t)(8 * v_max) - 1) / (uint32_t)(8 * v_max);
  size_t mcu_count = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_col, (size_t)mcu_per_row, &mcu_count)) {
    return GIMG_ERR_LIMIT;
  }

  static jpeg_derived_tbl dc_lum_tbl, dc_chr_tbl, ac_lum_tbl, ac_chr_tbl;
  static int tables_built = 0;
  if (!tables_built) {
    build_derived_tbl(gimg_jpeg_std_dc_lum_bits, gimg_jpeg_std_dc_lum_vals, 12, &dc_lum_tbl);
    build_derived_tbl(gimg_jpeg_std_dc_chr_bits, gimg_jpeg_std_dc_chr_vals, 12, &dc_chr_tbl);
    build_derived_tbl(gimg_jpeg_std_ac_lum_bits, gimg_jpeg_std_ac_lum_vals, 162, &ac_lum_tbl);
    build_derived_tbl(gimg_jpeg_std_ac_chr_bits, gimg_jpeg_std_ac_chr_vals, 162, &ac_chr_tbl);
    tables_built = 1;
  }

  jpeg_bit_writer w = {0};
  int last_dc[3] = {0, 0, 0};
  size_t block_off = 0;
  uint16_t next_restart = 0;
  size_t mcu_index = 0;

  // Optional: trace which symbol covers a given bit position (e.g. 247 for byte 30 LSB).
  unsigned long trace_bit = 247;
  const char * trace_env = NULL;
#if GIMG_JPEG_TRACE_BASELINE_BIT_POS
  trace_env = getenv("GIMG_JPEG_TRACE_BASELINE_BIT_POS");
  if (trace_env && trace_env[0] != '\0') {
    trace_bit = strtoul(trace_env, NULL, 0);
    if (trace_bit > 10000)
      trace_bit = 247;
  }
  else {
    trace_env = "1";
  }
#endif
  size_t total_bits = 0;

  for (;;) {
    // T.81 Annex F: RSTm (m = 0..7) at restart boundaries.
    if (restart_interval > 0 && mcu_index > 0 &&
        (mcu_index % (size_t)restart_interval) == 0) {
      bit_writer_flush(&w, alloc);
      if (!bit_writer_ensure(&w, alloc, 2)) {
        gimg_free(alloc, w.buf);
        return GIMG_ERR_OOM;
      }
      w.buf[w.len++] = 0xFF;
      w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
      next_restart++;
      w.bitbuf = 0;
      w.nbits = 0;
      last_dc[0] = 0;
      last_dc[1] = 0;
      last_dc[2] = 0;
    }

    for (int c = 0; c < num_components; c++) {
      const jpeg_derived_tbl * dc_tbl = (c == 0) ? &dc_lum_tbl : &dc_chr_tbl;
      const jpeg_derived_tbl * ac_tbl = (c == 0) ? &ac_lum_tbl : &ac_chr_tbl;
      size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
      for (size_t b = 0; b < nblocks; b++) {
        size_t block_idx = block_off + b;
        const int16_t * block = coef_buffer + block_idx * 64;
        int dc_val = (int)block[0];
        int diff = dc_val - last_dc[c];
        if (block_idx == 4 && getenv("GIMG_JPEG_TRACE_FIRST_CB")) {
          (void)fprintf(
              stderr, "BASELINE_ENC first Cb block_idx=4 dc_val=%d\n", dc_val);
          (void)fflush(stderr);
        }
        last_dc[c] = dc_val;
        int nbits = jpeg_nbits(diff);
        if (nbits > 11)
          nbits = 11;
        FILE * entropy_trace_fp = NULL;
#if GIMG_JPEG_TRACE_ENTROPY
        {
          const char * entropy_trace = getenv("GIMG_JPEG_TRACE_ENTROPY");
          if (entropy_trace && entropy_trace[0] != '\0') {
            entropy_trace_fp = fopen(entropy_trace, block_idx == 0 ? "w" : "a");
            if (entropy_trace_fp) {
              if (block_idx == 0 && ac_tbl->len[0] > 0) {
                (void)fprintf(entropy_trace_fp,
                    "table_ac_lum EOB code=0x%x len=%d sym03 code=0x%x "
                    "len=%d\n",
                    (unsigned)ac_tbl->code[0], ac_tbl->len[0],
                    (unsigned)ac_tbl->code[3], ac_tbl->len[3]);
              }
              (void)fprintf(entropy_trace_fp,
                  "block %zu DC cat=%d code=0x%x len=%d\n", block_idx, nbits,
                  (unsigned)dc_tbl->code[nbits], dc_tbl->len[nbits]);
            }
          }
        }
#endif
        if (dc_tbl->len[nbits] > 0) {
          int len = dc_tbl->len[nbits];
          if (trace_env && total_bits <= trace_bit &&
              (size_t)trace_bit < total_bits + (size_t)len) {
            (void)fprintf(stderr,
                "BASELINE_BIT_POS %lu: comp=%d block=%zu DC_code len=%d "
                "diff=%d\n",
                trace_bit, c, b, len, diff);
          }
          total_bits += (size_t)len;
          bit_writer_put_bits(&w, alloc, dc_tbl->code[nbits], len);
        }
        if (nbits > 0) {
          int extra = diff;
          if (extra < 0)
            extra += (1 << nbits) - 1;
          if (trace_env && total_bits <= trace_bit &&
              (size_t)trace_bit < total_bits + (size_t)nbits) {
            (void)fprintf(stderr,
                "BASELINE_BIT_POS %lu: comp=%d block=%zu DC_extra nbits=%d "
                "extra=%d\n",
                trace_bit, c, b, nbits, extra);
          }
          total_bits += (size_t)nbits;
          bit_writer_put_bits(&w, alloc, (unsigned int)extra, nbits);
        }
        // AC coefficients: coef_buffer is already in zigzag order (quantizer writes out[z]).
        // T.81 F.1.2.2: EOB is sent when the remaining coefficients are zero;
        // omit EOB when the last coefficient (zigzag 63) is nonzero (no "remaining" to signal).
        int k = 1;
        int ac_eob_emitted = 0;
        int last_encoded_was_63 = 0;
        while (k < 64) {
          int run = 0;
          while (k < 64 && block[k] == 0) {
            run++;
            k++;
          }
          if (k >= 64) {
            int len0 = ac_tbl->len[0];
            if (entropy_trace_fp)
              (void)fprintf(entropy_trace_fp,
                  "block %zu EOB code=0x%x len=%d\n", block_idx,
                  (unsigned)ac_tbl->code[0], len0);
            if (trace_env && total_bits <= trace_bit &&
                (size_t)trace_bit < total_bits + (size_t)len0) {
              (void)fprintf(stderr,
                  "BASELINE_BIT_POS %lu: comp=%d block=%zu AC_EOB\n", trace_bit,
                  c, b);
            }
            total_bits += (size_t)len0;
            bit_writer_put_bits(&w, alloc, ac_tbl->code[0], len0);
            ac_eob_emitted = 1;
            break;
          }
          if (run >= 16) {
            int len_f0 = ac_tbl->len[0xF0];
            if (entropy_trace_fp)
              (void)fprintf(entropy_trace_fp,
                  "block %zu AC run=15 size=0 ZRL sym=0xf0 code=0x%x len=%d\n",
                  block_idx, (unsigned)ac_tbl->code[0xF0], len_f0);
            if (trace_env && total_bits <= trace_bit &&
                (size_t)trace_bit < total_bits + (size_t)len_f0) {
              (void)fprintf(stderr,
                  "BASELINE_BIT_POS %lu: comp=%d block=%zu AC_ZRL\n", trace_bit,
                  c, b);
            }
            total_bits += (size_t)len_f0;
            bit_writer_put_bits(&w, alloc, ac_tbl->code[0xF0], len_f0);
            run -= 16;
            continue;
          }
          int coeff = (int)block[k];
          int size = jpeg_nbits(coeff);
          if (size > 10)
            size = 10;
          int symbol = (run << 4) | size;
          if (symbol < 0 || symbol > 255 || ac_tbl->len[symbol] == 0) {
            k++;
            continue;
          }
          int len_sym = ac_tbl->len[symbol];
          if (entropy_trace_fp)
            (void)fprintf(entropy_trace_fp,
                "block %zu AC run=%d size=%d sym=0x%02x code=0x%x len=%d\n",
                block_idx, run, size, symbol, (unsigned)ac_tbl->code[symbol],
                len_sym);
          if (trace_env && total_bits <= trace_bit &&
              (size_t)trace_bit < total_bits + (size_t)len_sym) {
            (void)fprintf(stderr,
                "BASELINE_BIT_POS %lu: comp=%d block=%zu AC run=%d size=%d "
                "zigzag_k=%d coeff=%d\n",
                trace_bit, c, b, run, size, (int)k, coeff);
          }
          total_bits += (size_t)len_sym;
          bit_writer_put_bits(&w, alloc, ac_tbl->code[symbol], len_sym);
          if (size > 0) {
            int extra = coeff;
            if (extra < 0)
              extra += (1 << size) - 1;
            if (trace_env && total_bits <= trace_bit &&
                (size_t)trace_bit < total_bits + (size_t)size) {
              (void)fprintf(stderr,
                  "BASELINE_BIT_POS %lu: comp=%d block=%zu AC_extra size=%d "
                  "extra=%d\n",
                  trace_bit, c, b, size, extra);
            }
            total_bits += (size_t)size;
            bit_writer_put_bits(&w, alloc, (unsigned int)extra, size);
          }
          last_encoded_was_63 = (k == 63);
          k++;
        }
        // T.81 Annex F: EOB when remaining are zero. Omit EOB when the last coefficient
        // (zigzag 63) was just encoded; emit EOB only when we exited via trailing zeros
        // or when we skipped a coefficient.
        if (k >= 64 && !ac_eob_emitted && !last_encoded_was_63) {
          int len0 = ac_tbl->len[0];
          if (entropy_trace_fp && len0 > 0)
            (void)fprintf(entropy_trace_fp, "block %zu EOB code=0x%x len=%d\n",
                block_idx, (unsigned)ac_tbl->code[0], len0);
          if (len0 > 0) {
            total_bits += (size_t)len0;
            bit_writer_put_bits(&w, alloc, ac_tbl->code[0], len0);
          }
        }
        if (entropy_trace_fp)
          (void)fclose(entropy_trace_fp);
      }
      block_off += (size_t)h_samp[c] * (size_t)v_samp[c];
    }

    mcu_index++;
    // T.81 Annex A: encode exactly the number of blocks in the coefficient buffer (from fill).
    // Break when we have encoded total_blocks blocks or completed mcu_count MCUs (defensive).
    if (block_off >= total_blocks || mcu_index >= mcu_count)
      break;
  }

  bit_writer_flush(&w, alloc);
  *out_scan_data = w.buf;
  *out_scan_size = w.len;
  return GIMG_OK;
}

/** Baseline sequential (single scan) from coefficient buffer using extended
 * DHT (DC 0..16, AC 242 symbols). T.81 Annex F: per block DC then AC.
 * Used for 12-bit SOF1 and any single-scan extended precision. */
GIMG_Result gimg_jpeg_encode_baseline_scan_from_coef_buffer_extended(
    uint32_t width, uint32_t height, int num_components,
    const int16_t * coef_buffer, size_t total_blocks, const uint8_t * h_samp,
    const uint8_t * v_samp, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size) {
  if (!alloc || !out_scan_data || !out_scan_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_scan_data = NULL;
  *out_scan_size = 0;
  static const uint8_t default_samp[3] = {1, 1, 1};
  if (!h_samp)
    h_samp = default_samp;
  if (!v_samp)
    v_samp = default_samp;
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max)
      h_max = h_samp[1];
    if (h_samp[2] > h_max)
      h_max = h_samp[2];
    if (v_samp[1] > v_max)
      v_max = v_samp[1];
    if (v_samp[2] > v_max)
      v_max = v_samp[2];
  }
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  uint32_t mcu_per_row =
      (width + (uint32_t)(8 * h_max) - 1) / (uint32_t)(8 * h_max);
  uint32_t mcu_per_col =
      (height + (uint32_t)(8 * v_max) - 1) / (uint32_t)(8 * v_max);
  size_t mcu_count = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_col, (size_t)mcu_per_row, &mcu_count)) {
    return GIMG_ERR_LIMIT;
  }
  static jpeg_derived_tbl ext_dc_lum_tbl, ext_dc_chr_tbl, ext_ac_lum_tbl,
      ext_ac_chr_tbl;
  static int ext_baseline_tables_built = 0;
  if (!ext_baseline_tables_built) {
    build_derived_tbl(gimg_jpeg_ext_dc_lum_bits, gimg_jpeg_ext_dc_lum_vals, GIMG_JPEG_EXT_DC_VALS,
        &ext_dc_lum_tbl);
    build_derived_tbl(gimg_jpeg_ext_dc_chr_bits, gimg_jpeg_ext_dc_chr_vals, GIMG_JPEG_EXT_DC_VALS,
        &ext_dc_chr_tbl);
    build_derived_tbl(gimg_jpeg_ext_ac_lum_bits, gimg_jpeg_ext_ac_lum_vals, GIMG_JPEG_EXT_AC_VALS,
        &ext_ac_lum_tbl);
    build_derived_tbl(gimg_jpeg_ext_ac_chr_bits, gimg_jpeg_ext_ac_chr_vals, GIMG_JPEG_EXT_AC_VALS,
        &ext_ac_chr_tbl);
    ext_baseline_tables_built = 1;
  }
  jpeg_bit_writer w = {0};
  int last_dc[3] = {0, 0, 0};
  size_t block_off = 0;
  uint16_t next_restart = 0;
  size_t mcu_index = 0;
  for (;;) {
    if (restart_interval > 0 && mcu_index > 0 &&
        (mcu_index % (size_t)restart_interval) == 0) {
      bit_writer_flush(&w, alloc);
      if (!bit_writer_ensure(&w, alloc, 2)) {
        gimg_free(alloc, w.buf);
        return GIMG_ERR_OOM;
      }
      w.buf[w.len++] = 0xFF;
      w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
      next_restart++;
      w.bitbuf = 0;
      w.nbits = 0;
      last_dc[0] = 0;
      last_dc[1] = 0;
      last_dc[2] = 0;
    }
    for (int c = 0; c < num_components; c++) {
      const jpeg_derived_tbl * dc_tbl =
          (c == 0) ? &ext_dc_lum_tbl : &ext_dc_chr_tbl;
      const jpeg_derived_tbl * ac_tbl =
          (c == 0) ? &ext_ac_lum_tbl : &ext_ac_chr_tbl;
      size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
      for (size_t b = 0; b < nblocks; b++) {
        size_t block_idx = block_off + b;
        const int16_t * block = coef_buffer + block_idx * 64;
        int dc_val = (int)block[0];
        int diff = dc_val - last_dc[c];
        last_dc[c] = dc_val;
        int nbits = jpeg_nbits(diff);
        if (nbits > 16)
          nbits = 16;
        if (dc_tbl->len[nbits] > 0) {
          bit_writer_put_bits(
              &w, alloc, dc_tbl->code[nbits], dc_tbl->len[nbits]);
        }
        if (nbits > 0) {
          int extra = diff;
          if (extra < 0)
            extra += (1 << nbits) - 1;
          bit_writer_put_bits(&w, alloc, (unsigned int)extra, nbits);
        }
        int k = 1;
        int ac_eob_emitted = 0;
        int last_encoded_was_63 = 0;
        while (k < 64) {
          int run = 0;
          while (k < 64 && block[k] == 0) {
            run++;
            k++;
          }
          if (k >= 64) {
            if (ac_tbl->len[0] > 0)
              bit_writer_put_bits(&w, alloc, ac_tbl->code[0], ac_tbl->len[0]);
            ac_eob_emitted = 1;
            break;
          }
          while (run >= 16) {
            if (ac_tbl->len[0xF0] > 0)
              bit_writer_put_bits(
                  &w, alloc, ac_tbl->code[0xF0], ac_tbl->len[0xF0]);
            run -= 16;
          }
          // T.81: do not encode past coefficient 63; EOB must be sent when rest are zero.
          if (k >= 64) {
            if (ac_tbl->len[0] > 0)
              bit_writer_put_bits(&w, alloc, ac_tbl->code[0], ac_tbl->len[0]);
            ac_eob_emitted = 1;
            break;
          }
          int coeff = (int)block[k];
          int size = jpeg_nbits(coeff);
          if (size > 15)
            size = 15;
          int symbol = (run << 4) | size;
          if (symbol >= 0 && symbol <= 255 && ac_tbl->len[symbol] > 0) {
            bit_writer_put_bits(
                &w, alloc, ac_tbl->code[symbol], ac_tbl->len[symbol]);
            if (size > 0) {
              int extra = coeff;
              if (extra < 0)
                extra += (1 << size) - 1;
              bit_writer_put_bits(&w, alloc, (unsigned int)extra, size);
            }
            last_encoded_was_63 = (k == 63);
          }
          k++;
        }
        if (k >= 64 && !ac_eob_emitted && !last_encoded_was_63 &&
            ac_tbl->len[0] > 0) {
          bit_writer_put_bits(&w, alloc, ac_tbl->code[0], ac_tbl->len[0]);
        }
      }
      block_off += (size_t)h_samp[c] * (size_t)v_samp[c];
    }
    mcu_index++;
    if (block_off >= total_blocks || mcu_index >= mcu_count)
      break;
  }
  bit_writer_flush(&w, alloc);
  *out_scan_data = w.buf;
  *out_scan_size = w.len;
  return GIMG_OK;
}

/** 12-bit variant: samples 0..4095, level shift 2048 (T.81). */
GIMG_Result gimg_jpeg_progressive_fill_coef_buffer_12bit(uint32_t width,
    uint32_t height, int num_components, const uint16_t * comp0,
    const uint16_t * comp1, const uint16_t * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, int16_t * coef_buffer,
    size_t * out_total_blocks) {
  static const uint8_t default_samp[3] = {1, 1, 1};
  if (!h_samp)
    h_samp = default_samp;
  if (!v_samp)
    v_samp = default_samp;
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max)
      h_max = h_samp[1];
    if (h_samp[2] > h_max)
      h_max = h_samp[2];
    if (v_samp[1] > v_max)
      v_max = v_samp[1];
    if (v_samp[2] > v_max)
      v_max = v_samp[2];
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  size_t total_blocks = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gcu_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
    return GIMG_ERR_LIMIT;
  }
  *out_total_blocks = total_blocks;

  const uint16_t * comps[3] = {comp0, comp1, comp2};
  size_t strides_el[3] = {stride0, stride1, stride2};
  uint32_t comp_w[3], comp_h[3];
  uint32_t comp_pix_w[3], comp_pix_h[3];
  for (int c = 0; c < num_components; c++) {
    jpeg_comp_blocks(width, height, h_samp[c], v_samp[c], h_max, v_max,
        &comp_w[c], &comp_h[c]);
    jpeg_comp_pixels(width, height, h_samp[c], v_samp[c], h_max, v_max,
        &comp_pix_w[c], &comp_pix_h[c]);
  }

  int16_t * out = coef_buffer;
  int32_t block[64];
  int16_t last_dc[3] = {0, 0, 0};
  const int32_t level_shift = 2048;

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      for (int c = 0; c < num_components; c++) {
        const uint16_t * quant = (c == 0) ? quant_luma : quant_chroma;
        const uint16_t * comp = comps[c];
        size_t stride_el = strides_el[c];
        uint32_t cw = comp_w[c];
        uint32_t ch = comp_h[c];
        uint32_t pix_w = comp_pix_w[c];
        uint32_t pix_h = comp_pix_h[c];
        for (uint32_t by = 0; by < (uint32_t)v_samp[c]; by++) {
          for (uint32_t bx = 0; bx < (uint32_t)h_samp[c]; bx++) {
            uint32_t blk_x = mcu_x * (uint32_t)h_samp[c] + bx;
            uint32_t blk_y = mcu_y * (uint32_t)v_samp[c] + by;
            if (blk_x >= cw || blk_y >= ch) {
              out[0] = last_dc[c];
              for (int i = 1; i < 64; i++)
                out[i] = 0;
              out += 64;
              continue;
            }
            uint32_t px = blk_x * 8;
            uint32_t py = blk_y * 8;
            for (int row = 0; row < 8; row++) {
              uint32_t y_src = (py + (uint32_t)row) < pix_h
                  ? (py + (uint32_t)row)
                  : (pix_h - 1u);
              size_t row_off = (size_t)y_src * stride_el;
              for (int col = 0; col < 8; col++) {
                uint32_t x_src = px + (uint32_t)col;
                if (x_src >= pix_w)
                  x_src = pix_w - 1u;
                uint16_t s = comp[row_off + (size_t)x_src];
                if (s > 4095u)
                  s = 4095u;
                block[row * 8 + col] = (int16_t)((int32_t)s - level_shift);
              }
            }
            jpeg_fdct_islow(block);
            jpeg_quantize_block_16bit(block, quant, out);
            last_dc[c] = out[0];
            out += 64;
          }
        }
      }
    }
  }
  return GIMG_OK;
}

// T.81 Annex G: progressive scan encode. DC scan (Ss=0,Se=0,Ah=0): encode only DC diff per block.
// AC initial (Ss>=1,Ah=0): encode AC in band [Ss,Se]. DC refinement (Ss=0,Se=0,Ah>0): one bit per block.
// AC refinement (Ah>0, Ss..Se): (run,size)+refinement/correction bits per T.81 G.1.2.2.
GIMG_Result gimg_jpeg_encode_progressive_scan(uint32_t width, uint32_t height,
    int num_components, const int16_t * coef_buffer, size_t total_blocks,
    const uint8_t * h_samp, const uint8_t * v_samp, uint8_t Ss, uint8_t Se,
    uint8_t Ah, uint8_t Al, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size, int16_t * state_after_scan_out,
    const int16_t * state_after_previous_scan, int sync_debug_scan_index) {
  (void)sync_debug_scan_index;
  if (!alloc || !out_scan_data || !out_scan_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_scan_data = NULL;
  *out_scan_size = 0;

  static const uint8_t default_samp[3] = {1, 1, 1};
  if (!h_samp)
    h_samp = default_samp;
  if (!v_samp)
    v_samp = default_samp;
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max)
      h_max = h_samp[1];
    if (h_samp[2] > h_max)
      h_max = h_samp[2];
    if (v_samp[1] > v_max)
      v_max = v_samp[1];
    if (v_samp[2] > v_max)
      v_max = v_samp[2];
  }
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  uint32_t mcu_per_row =
      (width + (uint32_t)(8 * h_max) - 1) / (uint32_t)(8 * h_max);
  uint32_t mcu_per_col =
      (height + (uint32_t)(8 * v_max) - 1) / (uint32_t)(8 * v_max);
  size_t mcu_count = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_col, (size_t)mcu_per_row, &mcu_count)) {
    return GIMG_ERR_LIMIT;
  }

  static jpeg_derived_tbl dc_lum_tbl, dc_chr_tbl, ac_lum_tbl, ac_chr_tbl;
  static int tables_built = 0;
  if (!tables_built) {
    build_derived_tbl(gimg_jpeg_std_dc_lum_bits, gimg_jpeg_std_dc_lum_vals, 12, &dc_lum_tbl);
    build_derived_tbl(gimg_jpeg_std_dc_chr_bits, gimg_jpeg_std_dc_chr_vals, 12, &dc_chr_tbl);
    build_derived_tbl(gimg_jpeg_std_ac_lum_bits, gimg_jpeg_std_ac_lum_vals, 162, &ac_lum_tbl);
    build_derived_tbl(gimg_jpeg_std_ac_chr_bits, gimg_jpeg_std_ac_chr_vals, 162, &ac_chr_tbl);
    tables_built = 1;
  }

  jpeg_bit_writer w = {0};
  int last_dc[3] = {0, 0, 0};
  size_t block_off = 0;
  uint16_t next_restart = 0;
  size_t mcu_index = 0;

  if (Ss == 0 && Se == 0) {
    if (Ah == 0) {
      // DC initial (T.81 Annex G.1.2.1): encode only DC difference per block.
      for (;;) {
        if (restart_interval > 0 && mcu_index > 0 &&
            (mcu_index % (size_t)restart_interval) == 0) {
          bit_writer_flush(&w, alloc);
          if (!bit_writer_ensure(&w, alloc, 2)) {
            gimg_free(alloc, w.buf);
            return GIMG_ERR_OOM;
          }
          w.buf[w.len++] = 0xFF;
          w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
          next_restart++;
          w.bitbuf = 0;
          w.nbits = 0;
          last_dc[0] = 0;
          last_dc[1] = 0;
          last_dc[2] = 0;
        }
        size_t mcu_block_off = 0;
        for (int c = 0; c < num_components; c++) {
          const jpeg_derived_tbl * dc_tbl =
              (c == 0) ? &dc_lum_tbl : &dc_chr_tbl;
          size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
          for (size_t b = 0; b < nblocks; b++) {
            size_t block_idx = block_off + mcu_block_off + b;
            const int16_t * block = coef_buffer + block_idx * 64;
            int dc_val = (int)block[0];
            int diff = dc_val - last_dc[c];
            if (block_idx < 6 && GIMG_JPEG_TRACE_PROG_FIRST_DC) {
              (void)fprintf(stderr,
                  "PROG_ENC_DC block=%zu c=%d dc_val=%d diff=%d\n", block_idx, c,
                  dc_val, diff);
              (void)fflush(stderr);
            }
            if (block_idx == 4 && GIMG_JPEG_TRACE_FIRST_CB) {
              (void)fprintf(
                  stderr, "PROG_ENC first Cb block_idx=4 dc_val=%d\n", dc_val);
              (void)fflush(stderr);
            }
            last_dc[c] = dc_val;
            int nbits = jpeg_nbits(diff);
            if (nbits > 11)
              nbits = 11;
            if (dc_tbl->len[nbits] > 0) {
              bit_writer_put_bits(
                  &w, alloc, dc_tbl->code[nbits], dc_tbl->len[nbits]);
            }
            if (nbits > 0) {
              int extra = diff;
              if (extra < 0)
                extra += (1 << nbits) - 1;
              bit_writer_put_bits(&w, alloc, (unsigned int)extra, nbits);
            }
          }
          mcu_block_off += nblocks;
        }
        block_off += blocks_per_mcu;
        mcu_index++;
        if (block_off >= total_blocks || mcu_index >= mcu_count)
          break;
      }
    }
    else {
      // DC refinement (T.81 Annex G.1.1.2.1): one bit per block (Al-th bit of DC).
      // Decoder sets block[0] |= (bit << Al).
      unsigned int bitpos = (Al <= 15u) ? Al : 0;
      for (;;) {
        if (restart_interval > 0 && mcu_index > 0 &&
            (mcu_index % (size_t)restart_interval) == 0) {
          bit_writer_flush(&w, alloc);
          if (!bit_writer_ensure(&w, alloc, 2)) {
            gimg_free(alloc, w.buf);
            return GIMG_ERR_OOM;
          }
          w.buf[w.len++] = 0xFF;
          w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
          next_restart++;
          w.bitbuf = 0;
          w.nbits = 0;
        }
        size_t mcu_block_off = 0;
        for (int c = 0; c < num_components; c++) {
          size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
          for (size_t b = 0; b < nblocks; b++) {
            size_t block_idx = block_off + mcu_block_off + b;
            const int16_t * block = coef_buffer + block_idx * 64;
            int dc = (int)block[0];
            int mag = dc < 0 ? -dc : dc;
            unsigned int bit = (unsigned int)((mag >> bitpos) & 1);
            bit_writer_put_bits(&w, alloc, bit, 1);
          }
          mcu_block_off += nblocks;
        }
        block_off += blocks_per_mcu;
        mcu_index++;
        if (block_off >= total_blocks || mcu_index >= mcu_count)
          break;
      }
    }
  }
  else if (Ah != 0) {
    // AC refinement (T.81 Annex G.1.2.2): (run,size=1) + refinement bit for newly nonzero;
    // correction bits for already-nonzero. state_after_previous is coefficient state after AC initial.
    const int16_t * prev = state_after_previous_scan;
    if (!prev) {
      gimg_free(alloc, w.buf);
      return GIMG_ERR_UNSUPPORTED;
    }
    static jpeg_derived_tbl ac_refine_tbl;
    static int ac_refine_tbl_built = 0;
    if (!ac_refine_tbl_built) {
      build_derived_tbl(gimg_jpeg_std_ac_refine_bits, gimg_jpeg_std_ac_refine_vals,
        GIMG_JPEG_AC_REFINE_VALS, &ac_refine_tbl);
      ac_refine_tbl_built = 1;
    }
    unsigned int k_start = (unsigned int)Ss;
    unsigned int k_end = (unsigned int)Se;
    if (k_end > 63)
      k_end = 63;
    int bitpos = (Al <= 15u) ? (int)Al : 0;
    for (;;) {
      if (restart_interval > 0 && mcu_index > 0 &&
          (mcu_index % (size_t)restart_interval) == 0) {
        bit_writer_flush(&w, alloc);
        if (!bit_writer_ensure(&w, alloc, 2)) {
          gimg_free(alloc, w.buf);
          return GIMG_ERR_OOM;
        }
        w.buf[w.len++] = 0xFF;
        w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
        next_restart++;
        w.bitbuf = 0;
        w.nbits = 0;
      }
      size_t mcu_block_off = 0;
      for (int c = 0; c < num_components; c++) {
        size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
        for (size_t b = 0; b < nblocks; b++) {
          size_t block_idx = block_off + mcu_block_off + b;
          const int16_t * block = coef_buffer + block_idx * 64;
          const int16_t * p = prev + block_idx * 64;
          unsigned int start = k_start;
          for (unsigned int k = k_start; k <= k_end;) {
            int coef = (int)block[k];
            int prev_val = (int)p[k];
            int mag = coef < 0 ? -coef : coef;
            int newly_nz = (prev_val == 0 && mag != 0 &&
                (mag & (1 << bitpos)) != 0);
            if (newly_nz) {
              int run = (int)(k - start);
              while (run >= 16) {
                if (ac_refine_tbl.len[0xF0] > 0)
                  bit_writer_put_bits(&w, alloc, ac_refine_tbl.code[0xF0],
                      ac_refine_tbl.len[0xF0]);
                run -= 16;
                start += 16;
              }
              int symbol = (run << 4) | 1;
              if (symbol >= 0 && symbol <= 255 &&
                  ac_refine_tbl.len[symbol] > 0) {
                bit_writer_put_bits(&w, alloc, ac_refine_tbl.code[symbol],
                    ac_refine_tbl.len[symbol]);
              }
              unsigned int ref_bit = (coef > 0) ? 1u : 0u;
              bit_writer_put_bits(&w, alloc, ref_bit, 1);
              for (unsigned int pos = start; pos < k; pos++) {
                if (p[pos] != 0) {
                  int cmag = (int)block[pos];
                  if (cmag < 0)
                    cmag = -cmag;
                  unsigned int corr =
                      (unsigned int)((cmag >> bitpos) & 1);
                  bit_writer_put_bits(&w, alloc, corr, 1);
                }
              }
              start = k + 1;
              k++;
            }
            else if (prev_val != 0) {
              k++;
            }
            else {
              k++;
            }
          }
          if (ac_refine_tbl.len[0] > 0)
            bit_writer_put_bits(&w, alloc, ac_refine_tbl.code[0],
                ac_refine_tbl.len[0]);
          for (unsigned int pos = start; pos <= k_end; pos++) {
            if (p[pos] != 0) {
              int cmag = (int)block[pos];
              if (cmag < 0)
                cmag = -cmag;
              unsigned int corr = (unsigned int)((cmag >> bitpos) & 1);
              bit_writer_put_bits(&w, alloc, corr, 1);
            }
          }
        }
        mcu_block_off += nblocks;
      }
      block_off += blocks_per_mcu;
      mcu_index++;
      if (block_off >= total_blocks || mcu_index >= mcu_count)
        break;
    }
  }
  else {
    // AC initial scan (T.81 Annex G.1.2.2): encode AC in band [Ss, Se].
    unsigned int k_start = (unsigned int)Ss;
    unsigned int k_end = (unsigned int)Se;
    if (k_end > 63)
      k_end = 63;
    for (;;) {
      if (restart_interval > 0 && mcu_index > 0 &&
          (mcu_index % (size_t)restart_interval) == 0) {
        bit_writer_flush(&w, alloc);
        if (!bit_writer_ensure(&w, alloc, 2)) {
          gimg_free(alloc, w.buf);
          return GIMG_ERR_OOM;
        }
        w.buf[w.len++] = 0xFF;
        w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
        next_restart++;
        w.bitbuf = 0;
        w.nbits = 0;
      }
      size_t mcu_block_off = 0;
      for (int c = 0; c < num_components; c++) {
        const jpeg_derived_tbl * ac_tbl = (c == 0) ? &ac_lum_tbl : &ac_chr_tbl;
        size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
        for (size_t b = 0; b < nblocks; b++) {
          size_t block_idx = block_off + mcu_block_off + b;
          const int16_t * block = coef_buffer + block_idx * 64;
          if (state_after_scan_out) {
            for (int i = 0; i < 64; i++)
              state_after_scan_out[block_idx * 64 + i] = 0;
          }
          for (unsigned int k = k_start; k <= k_end;) {
            int run = 0;
            while (k <= k_end && block[k] == 0) {
              run++;
              k++;
            }
            if (k > k_end) {
              if (block_idx == 0 && GIMG_JPEG_TRACE_PROG_FIRST_AC) {
                (void)fprintf(stderr, "PROG_ENC_AC block=0 EOB\n");
                (void)fflush(stderr);
              }
              if (ac_tbl->len[0] > 0)
                bit_writer_put_bits(&w, alloc, ac_tbl->code[0], ac_tbl->len[0]);
              break;
            }
            while (run >= 16) {
              if (ac_tbl->len[0xF0] > 0)
                bit_writer_put_bits(
                    &w, alloc, ac_tbl->code[0xF0], ac_tbl->len[0xF0]);
              run -= 16;
            }
            int coeff = (int)block[k];
            int size = jpeg_nbits(coeff);
            if (size > 10)
              size = 10;
            int symbol = (run << 4) | size;
            if (symbol >= 0 && symbol <= 255 && ac_tbl->len[symbol] > 0) {
              if (block_idx == 0 && GIMG_JPEG_TRACE_PROG_FIRST_AC) {
                (void)fprintf(stderr,
                    "PROG_ENC_AC block=0 run=%d size=%d val=%d k=%u\n", run,
                    size, coeff, k);
                (void)fflush(stderr);
              }
              bit_writer_put_bits(
                  &w, alloc, ac_tbl->code[symbol], ac_tbl->len[symbol]);
              if (size > 0) {
                int extra = coeff;
                if (extra < 0)
                  extra += (1 << size) - 1;
                bit_writer_put_bits(&w, alloc, (unsigned int)extra, size);
              }
              if (state_after_scan_out)
                state_after_scan_out[block_idx * 64 + k] = (int16_t)coeff;
            }
            k++;
          }
        }
        mcu_block_off += nblocks;
      }
      block_off += blocks_per_mcu;
      mcu_index++;
      if (block_off >= total_blocks || mcu_index >= mcu_count)
        break;
    }
  }

  bit_writer_flush(&w, alloc);
  *out_scan_data = w.buf;
  *out_scan_size = w.len;
  return GIMG_OK;
}

// 16-bit progressive: DC size 0..16, AC 242 symbols. No refinement (Ah!=0).
GIMG_Result gimg_jpeg_encode_progressive_scan_extended(uint32_t width,
    uint32_t height, int num_components, const int16_t * coef_buffer,
    size_t total_blocks, const uint8_t * h_samp, const uint8_t * v_samp,
    uint8_t Ss, uint8_t Se, uint8_t Ah, uint8_t Al,
    const GIMG_Allocator * alloc, uint16_t restart_interval,
    unsigned char ** out_scan_data, size_t * out_scan_size) {
  (void)Al;
  if (!alloc || !out_scan_data || !out_scan_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_scan_data = NULL;
  *out_scan_size = 0;
  if (Ah != 0) {
    return GIMG_ERR_UNSUPPORTED;
  }
  static const uint8_t default_samp[3] = {1, 1, 1};
  if (!h_samp)
    h_samp = default_samp;
  if (!v_samp)
    v_samp = default_samp;
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max)
      h_max = h_samp[1];
    if (h_samp[2] > h_max)
      h_max = h_samp[2];
    if (v_samp[1] > v_max)
      v_max = v_samp[1];
    if (v_samp[2] > v_max)
      v_max = v_samp[2];
  }
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  uint32_t mcu_per_row =
      (width + (uint32_t)(8 * h_max) - 1) / (uint32_t)(8 * h_max);
  uint32_t mcu_per_col =
      (height + (uint32_t)(8 * v_max) - 1) / (uint32_t)(8 * v_max);
  size_t mcu_count = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_col, (size_t)mcu_per_row, &mcu_count)) {
    return GIMG_ERR_LIMIT;
  }

  static jpeg_derived_tbl ext_dc_lum_tbl, ext_dc_chr_tbl, ext_ac_lum_tbl,
      ext_ac_chr_tbl;
  static int ext_tables_built = 0;
  if (!ext_tables_built) {
    build_derived_tbl(gimg_jpeg_ext_dc_lum_bits, gimg_jpeg_ext_dc_lum_vals, GIMG_JPEG_EXT_DC_VALS,
        &ext_dc_lum_tbl);
    build_derived_tbl(gimg_jpeg_ext_dc_chr_bits, gimg_jpeg_ext_dc_chr_vals, GIMG_JPEG_EXT_DC_VALS,
        &ext_dc_chr_tbl);
    build_derived_tbl(gimg_jpeg_ext_ac_lum_bits, gimg_jpeg_ext_ac_lum_vals, GIMG_JPEG_EXT_AC_VALS,
        &ext_ac_lum_tbl);
    build_derived_tbl(gimg_jpeg_ext_ac_chr_bits, gimg_jpeg_ext_ac_chr_vals, GIMG_JPEG_EXT_AC_VALS,
        &ext_ac_chr_tbl);
    ext_tables_built = 1;
  }

  jpeg_bit_writer w = {0};
  int last_dc[3] = {0, 0, 0};
  size_t block_off = 0;
  uint16_t next_restart = 0;
  size_t mcu_index = 0;

  if (Ss == 0 && Se == 0) {
    for (;;) {
      if (restart_interval > 0 && mcu_index > 0 &&
          (mcu_index % (size_t)restart_interval) == 0) {
        bit_writer_flush(&w, alloc);
        if (!bit_writer_ensure(&w, alloc, 2)) {
          gimg_free(alloc, w.buf);
          return GIMG_ERR_OOM;
        }
        w.buf[w.len++] = 0xFF;
        w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
        next_restart++;
        w.bitbuf = 0;
        w.nbits = 0;
        last_dc[0] = 0;
        last_dc[1] = 0;
        last_dc[2] = 0;
      }
      size_t mcu_block_off = 0;
      for (int c = 0; c < num_components; c++) {
        const jpeg_derived_tbl * dc_tbl =
            (c == 0) ? &ext_dc_lum_tbl : &ext_dc_chr_tbl;
        size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
        for (size_t b = 0; b < nblocks; b++) {
          size_t block_idx = block_off + mcu_block_off + b;
          const int16_t * block = coef_buffer + block_idx * 64;
          int dc_val = (int)block[0];
          int diff = dc_val - last_dc[c];
          last_dc[c] = dc_val;
          int nbits = jpeg_nbits(diff);
          if (nbits > 16)
            nbits = 16;
          if (dc_tbl->len[nbits] > 0) {
            bit_writer_put_bits(
                &w, alloc, dc_tbl->code[nbits], dc_tbl->len[nbits]);
          }
          if (nbits > 0) {
            int extra = diff;
            if (extra < 0)
              extra += (1 << nbits) - 1;
            bit_writer_put_bits(&w, alloc, (unsigned int)extra, nbits);
          }
        }
        mcu_block_off += nblocks;
      }
      block_off += blocks_per_mcu;
      mcu_index++;
      if (block_off >= total_blocks || mcu_index >= mcu_count)
        break;
    }
  }
  else {
    unsigned int k_start = (unsigned int)Ss;
    unsigned int k_end = (unsigned int)Se;
    if (k_end > 63)
      k_end = 63;
    for (;;) {
      if (restart_interval > 0 && mcu_index > 0 &&
          (mcu_index % (size_t)restart_interval) == 0) {
        bit_writer_flush(&w, alloc);
        if (!bit_writer_ensure(&w, alloc, 2)) {
          gimg_free(alloc, w.buf);
          return GIMG_ERR_OOM;
        }
        w.buf[w.len++] = 0xFF;
        w.buf[w.len++] = (unsigned char)(0xD0 + (next_restart & 7));
        next_restart++;
        w.bitbuf = 0;
        w.nbits = 0;
      }
      size_t mcu_block_off = 0;
      for (int c = 0; c < num_components; c++) {
        const jpeg_derived_tbl * ac_tbl =
            (c == 0) ? &ext_ac_lum_tbl : &ext_ac_chr_tbl;
        size_t nblocks = (size_t)h_samp[c] * (size_t)v_samp[c];
        for (size_t b = 0; b < nblocks; b++) {
          size_t block_idx = block_off + mcu_block_off + b;
          const int16_t * block = coef_buffer + block_idx * 64;
          for (unsigned int k = k_start; k <= k_end;) {
            int run = 0;
            while (k <= k_end && block[k] == 0) {
              run++;
              k++;
            }
            if (k > k_end) {
              if (ac_tbl->len[0] > 0)
                bit_writer_put_bits(&w, alloc, ac_tbl->code[0], ac_tbl->len[0]);
              break;
            }
            while (run >= 16) {
              if (ac_tbl->len[0xF0] > 0)
                bit_writer_put_bits(
                    &w, alloc, ac_tbl->code[0xF0], ac_tbl->len[0xF0]);
              run -= 16;
            }
            int coeff = (int)block[k];
            int size = jpeg_nbits(coeff);
            if (size > 15)
              size = 15;
            int symbol = (run << 4) | size;
            if (symbol >= 0 && symbol <= 255 && ac_tbl->len[symbol] > 0) {
              bit_writer_put_bits(
                  &w, alloc, ac_tbl->code[symbol], ac_tbl->len[symbol]);
              if (size > 0) {
                int extra = coeff;
                if (extra < 0)
                  extra += (1 << size) - 1;
                bit_writer_put_bits(&w, alloc, (unsigned int)extra, size);
              }
            }
            k++;
          }
        }
        mcu_block_off += nblocks;
      }
      block_off += blocks_per_mcu;
      mcu_index++;
      if (block_off >= total_blocks || mcu_index >= mcu_count)
        break;
    }
  }

  bit_writer_flush(&w, alloc);
  *out_scan_data = w.buf;
  *out_scan_size = w.len;
  return GIMG_OK;
}
