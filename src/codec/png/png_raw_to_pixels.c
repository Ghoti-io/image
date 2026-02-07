/**
 * @file
 *
 * PNG raw sample buffer to output-format pixels. Shared by single-frame and
 * APNG decode. Converts unfiltered raw rows (from DEFLATE + filter reversal)
 * to GIMG_Pixel_Format layout (GRAY8/16, RGBA8/16).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/raster.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "png_internal.h"

/** Read 16-bit big-endian sample from buffer. */
static uint16_t gimg_png_read_be16(const unsigned char * p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/** Scale a sample from 1/2/4-bit to 8-bit (PNG sample range to 0..255). */
static unsigned char gimg_png_scale_to_8(unsigned int sample, uint8_t depth) {
  if (depth >= 8) {
    return (unsigned char)sample;
  }
  unsigned int max_val = (1u << depth) - 1u;
  if (max_val == 0) {
    return (unsigned char)(sample ? 255 : 0);
  }
  return (unsigned char)((sample * 255u + max_val / 2u) / max_val);
}

/**
 * Get one sample (channel) at x from a decoded row. For depth 1/2/4 samples are
 * packed; for 8 one byte; for 16 two bytes big-endian. Returns value in
 * 0..(2^depth - 1) for depth <= 8, or 0..65535 for depth 16.
 */
static uint32_t gimg_png_sample_at(
    const unsigned char * row, uint32_t x, uint8_t depth) {
  if (depth == 8) {
    return (uint32_t)row[x];
  }
  if (depth == 16) {
    return (uint32_t)gimg_png_read_be16(row + (size_t)x * 2u);
  }
  if (depth == 1) {
    size_t byte_ix = (size_t)x / 8u;
    unsigned int bit = 7 - (unsigned int)(x % 8u);
    return (uint32_t)((row[byte_ix] >> bit) & 1u);
  }
  if (depth == 2) {
    size_t byte_ix = (size_t)x / 4u;
    unsigned int shift = 6 - 2u * (unsigned int)(x % 4u);
    return (uint32_t)((row[byte_ix] >> shift) & 3u);
  }
  if (depth == 4) {
    size_t byte_ix = (size_t)x / 2u;
    uint32_t v = (uint32_t)row[byte_ix];
    return (x & 1u) ? (v & 15u) : (v >> 4u);
  }
  return 0;
}

// Convert unfiltered raw rows (full image, possibly from Adam7 reassembly) to
// output pixel format. color_type 0/2/3/4/6 map to GRAY8/16 or RGBA8/16; tRNS
// is applied for grayscale/palette when present; palette uses PLTE + optional
// tRNS. Sample depths 1/2/4 are scaled to 8-bit; 16-bit stays big-endian in
// raster. Called once per frame from decode path after DEFLATE + unfilter.
void gimg_png_raw_full_to_pixels(const gimg_png_doc_state_t * state,
    const gimg_png_ihdr_t * ihdr, const GIMG_Pixel_Format * format,
    const unsigned char * raw_full, uint32_t w, uint32_t h, size_t row_bytes,
    void * pixels, size_t stride) {
  bool use_trns = (state->trns && state->trns_size > 0);
  uint8_t depth = ihdr->bit_depth;

  if (ihdr->color_type == 0) {
    uint16_t trns_gray = 0;
    if (use_trns && state->trns_size >= 2) {
      trns_gray = gimg_png_read_be16(state->trns);
    }
    if (format == &GIMG_PIXEL_GRAY8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t v = gimg_png_sample_at(src, x, depth);
          dst[x] = gimg_png_scale_to_8((unsigned int)v, depth);
        }
      }
    }
    else if (format == &GIMG_PIXEL_GRAY16) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t v = gimg_png_sample_at(src, x, depth);
          uint16_t le = (uint16_t)v;
          dst[x * 2u] = (unsigned char)(le & 0xFFu);
          dst[x * 2u + 1u] = (unsigned char)(le >> 8);
        }
      }
    }
    else {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t v = gimg_png_sample_at(src, x, depth);
          unsigned char g8 = depth <= 8
              ? gimg_png_scale_to_8((unsigned int)v, depth)
              : (unsigned char)((v >> 8) & 0xFFu);
          uint16_t v16 = (uint16_t)v;
          bool match = use_trns && (v16 == trns_gray);
          if (format == &GIMG_PIXEL_RGBA8) {
            dst[0] = g8;
            dst[1] = g8;
            dst[2] = g8;
            dst[3] = (unsigned char)(match ? 0 : 255);
            dst += 4;
          }
          else {
            uint16_t a16 = (uint16_t)(match ? 0 : 65535);
            dst[0] = (unsigned char)(v16 & 0xFFu);
            dst[1] = (unsigned char)(v16 >> 8);
            dst[2] = (unsigned char)(v16 & 0xFFu);
            dst[3] = (unsigned char)(v16 >> 8);
            dst[4] = (unsigned char)(v16 & 0xFFu);
            dst[5] = (unsigned char)(v16 >> 8);
            dst[6] = (unsigned char)(a16 & 0xFFu);
            dst[7] = (unsigned char)(a16 >> 8);
            dst += 8;
          }
        }
      }
    }
  }
  else if (ihdr->color_type == 2) {
    uint16_t trns_r = 0, trns_g = 0, trns_b = 0;
    bool has_trns = false;
    if (use_trns && state->trns_size >= 6) {
      trns_r = gimg_png_read_be16(state->trns);
      trns_g = gimg_png_read_be16(state->trns + 2);
      trns_b = gimg_png_read_be16(state->trns + 4);
      has_trns = true;
    }
    if (depth == 8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          unsigned char r = src[0], g = src[1], b = src[2];
          bool match = has_trns &&
              (r == (trns_r & 0xFF) && g == (trns_g & 0xFF) &&
                  b == (trns_b & 0xFF));
          dst[0] = r;
          dst[1] = g;
          dst[2] = b;
          dst[3] = (unsigned char)(match ? 0 : 255);
          src += 3;
          dst += 4;
        }
      }
    }
    else {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint16_t r = gimg_png_read_be16(src), g = gimg_png_read_be16(src + 2),
                   b = gimg_png_read_be16(src + 4);
          bool match = has_trns && (r == trns_r && g == trns_g && b == trns_b);
          dst[0] = (unsigned char)(r & 0xFFu);
          dst[1] = (unsigned char)(r >> 8);
          dst[2] = (unsigned char)(g & 0xFFu);
          dst[3] = (unsigned char)(g >> 8);
          dst[4] = (unsigned char)(b & 0xFFu);
          dst[5] = (unsigned char)(b >> 8);
          dst[6] = (unsigned char)(match ? 0 : 255);
          dst[7] = (unsigned char)(match ? 0 : 255);
          src += 6;
          dst += 8;
        }
      }
    }
  }
  else if (ihdr->color_type == 3) {
    for (uint32_t y = 0; y < h; y++) {
      const unsigned char * src = raw_full + (size_t)y * row_bytes;
      unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
      for (uint32_t x = 0; x < w; x++) {
        unsigned int idx = (unsigned int)gimg_png_sample_at(src, x, depth);
        if (idx >= GIMG_PNG_PLTE_MAX_ENTRIES) {
          idx = GIMG_PNG_PLTE_MAX_ENTRIES - 1u;
        }
        size_t off = (size_t)idx * 3u;
        dst[0] = off + 0 < state->plte_size ? state->plte[off + 0] : 0;
        dst[1] = off + 1 < state->plte_size ? state->plte[off + 1] : 0;
        dst[2] = off + 2 < state->plte_size ? state->plte[off + 2] : 0;
        dst[3] = (state->trns && (size_t)idx < state->trns_size)
            ? state->trns[idx]
            : (unsigned char)255;
        dst += 4;
      }
    }
  }
  else if (ihdr->color_type == 4) {
    if (depth == 8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          unsigned char g = src[0], a = src[1];
          dst[0] = dst[1] = dst[2] = g;
          dst[3] = a;
          src += 2;
          dst += 4;
        }
      }
    }
    else {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint16_t g = gimg_png_read_be16(src), a = gimg_png_read_be16(src + 2);
          dst[0] = (unsigned char)(g & 0xFFu);
          dst[1] = (unsigned char)(g >> 8);
          dst[2] = (unsigned char)(g & 0xFFu);
          dst[3] = (unsigned char)(g >> 8);
          dst[4] = (unsigned char)(g & 0xFFu);
          dst[5] = (unsigned char)(g >> 8);
          dst[6] = (unsigned char)(a & 0xFFu);
          dst[7] = (unsigned char)(a >> 8);
          src += 4;
          dst += 8;
        }
      }
    }
  }
  else if (ihdr->color_type == 6) {
    if (depth == 8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        memcpy(dst, src, (size_t)w * 4u);
      }
    }
    else {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          dst[0] = (unsigned char)(gimg_png_read_be16(src) & 0xFFu);
          dst[1] = (unsigned char)(gimg_png_read_be16(src) >> 8);
          dst[2] = (unsigned char)(gimg_png_read_be16(src + 2) & 0xFFu);
          dst[3] = (unsigned char)(gimg_png_read_be16(src + 2) >> 8);
          dst[4] = (unsigned char)(gimg_png_read_be16(src + 4) & 0xFFu);
          dst[5] = (unsigned char)(gimg_png_read_be16(src + 4) >> 8);
          dst[6] = (unsigned char)(gimg_png_read_be16(src + 6) & 0xFFu);
          dst[7] = (unsigned char)(gimg_png_read_be16(src + 6) >> 8);
          src += 8;
          dst += 8;
        }
      }
    }
  }
}
