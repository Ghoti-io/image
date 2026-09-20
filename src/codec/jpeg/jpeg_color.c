/**
 * @file
 *
 * JPEG color: what a decoded raster is told about the meaning of its samples.
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Two things a JPEG can say end up on GIMG_Color_Info: the ICC profile its
 * APP2 segments carry, and - for a four-component frame - the polarity of its
 * ink amounts.  Both are properties of the file, not of the coding process
 * that produced it, but each decode path used to attach them for itself and
 * no two of them agreed: the baseline path set both, the hierarchical path
 * set only the polarity, and the extended and progressive-extended paths set
 * only the profile.  So the same CMYK image came back saying 0 is full ink
 * when it was baseline and saying nothing at all when it was progressive or
 * twelve-bit, which is a difference a consumer would render as an inverted
 * picture.
 *
 * The polarity is GIMG_CMYK_POLARITY_INK for every four-component frame this
 * decoder emits.  That is the Adobe convention the format is written in and
 * what libjpeg assumes (jdapimin.c); a file that meant the other one has no
 * way to say so, so this is a statement about JPEG rather than a guess about
 * the image.
 */

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>

#include "../../raster/raster_internal.h"
#include "jpeg_internal.h"

void gimg_jpeg_attach_color(const gimg_jpeg_doc_state_t * state,
    uint8_t num_comp, GIMG_Raster * raster) {
  if (!raster) {
    return;
  }
  bool have_icc = state && state->app2_icc && state->app2_icc_len > 0u;
  if (num_comp != 4u && !have_icc) {
    return;
  }

  GIMG_Color_Info color_info;
  gimg_color_info_default(&color_info);
  if (num_comp == 4u) {
    color_info.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
  }
  if (have_icc) {
    // A profile assembled from several APP2 segments is stored bare; one that
    // arrived in a single segment is stored with that segment's 14-byte
    // ICC_PROFILE header still in front of it.
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
  }
  (void)gimg_raster_set_color_info(raster, &color_info);
}

GIMG_Result gimg_jpeg_cmyk_to_file_polarity(
    const GIMG_Raster * raster, GIMG_Raster ** out_raster) {
  *out_raster = NULL;
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->channel_model != GIMG_CHANNEL_CMYK ||
      fmt->channel_count != 4 || fmt->layout != GIMG_LAYOUT_INTERLEAVED) {
    return GIMG_OK;
  }
  const GIMG_Color_Info * ci = gimg_raster_color_info_const(raster);
  if (!ci || ci->cmyk_polarity != GIMG_CMYK_POLARITY_REFLECTION) {
    // INK is what a JPEG means, and an unstated polarity is taken to be the
    // file's own convention rather than refused: a caller building CMYK
    // samples for a JPEG is building them the way a JPEG holds them.  Only
    // the other reading needs anything done to it.
    return GIMG_OK;
  }

  uint8_t bits = fmt->bits_per_channel[0];
  if (bits != 8 && bits != 12 && bits != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t max = (bits == 8) ? 255u : ((UINT32_C(1) << bits) - 1u);
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  GIMG_Result r =
      gimg_raster_create_with_allocator(gimg_raster_allocator(raster), w, h,
          fmt, GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    return r;
  }

  size_t src_stride = gimg_raster_stride_bytes(raster);
  size_t dst_stride = gimg_raster_stride_bytes(*out_raster);
  const unsigned char * sp =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  unsigned char * dp = (unsigned char *)gimg_raster_pixels(*out_raster);
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * sr = sp + ((size_t)y * src_stride);
    unsigned char * dr = dp + ((size_t)y * dst_stride);
    for (size_t x = 0; x < (size_t)w * 4u; x++) {
      if (bits == 8) {
        dr[x] = (unsigned char)(max - sr[x]);
      }
      else {
        uint32_t v = ((const uint16_t *)sr)[x];
        if (v > max) {
          v = max;
        }
        ((uint16_t *)dr)[x] = (uint16_t)(max - v);
      }
    }
  }

  // The samples now mean what the file will mean, so say so.  Everything else
  // the raster said about its colour still holds - the profile above all,
  // which describes the same ink amounts either way round.
  GIMG_Color_Info flipped = *ci;
  flipped.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
  r = gimg_raster_set_color_info(*out_raster, &flipped);
  if (r != GIMG_OK) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
  return r;
}
