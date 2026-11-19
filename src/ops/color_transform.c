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
 * Explicit colour transform: samples move only when the caller asks.
 *
 * Load and save carry GCOL_Color_Info and opaque ICC bytes. This file is the
 * CMM seam into libs/color — the same shape as Pillow ImageCms and WIC's
 * IWICColorTransform.
 */

#include <ghoti.io/color/color.h>
#include <ghoti.io/color/icc.h>
#include <ghoti.io/color/transform.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <stdlib.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "../core/limits_guard_internal.h"
#include "../raster/raster_internal.h"
#include "ops_internal.h"

/** ICC data-space signatures from the profile header (four-character codes). */
#define GIMG_ICC_SIG_RGB 0x52474220u  /* 'RGB ' */
#define GIMG_ICC_SIG_CMYK 0x434D594Bu /* 'CMYK' */
#define GIMG_ICC_SIG_GRAY 0x47524159u /* 'GRAY' */

static GIMG_Result gimg_map_gcol_result(GCOL_Result r) {
  switch (r) {
  case GCOL_OK:
    return GIMG_OK;
  case GCOL_ERR_OOM:
    return GIMG_ERR_OOM;
  case GCOL_ERR_LIMIT:
    return GIMG_ERR_LIMIT;
  case GCOL_ERR_CORRUPT:
    return GIMG_ERR_CORRUPT;
  case GCOL_ERR_UNSUPPORTED:
  case GCOL_ERR_FORMAT:
  case GCOL_ERR_INVALID:
    return GIMG_ERR_UNSUPPORTED;
  case GCOL_ERR_IO:
  case GCOL_ERR_INTERNAL:
  default:
    return GIMG_ERR_INTERNAL;
  }
}

/**
 * A gray description: a white and a transfer and no primaries, which is
 * what gcol_icc_to_color_info gives for a gray profile.
 */
static bool gimg_color_info_gray_ready(const GCOL_Color_Info * info) {
  return info && !info->primaries_stated && info->white_stated &&
      info->transfer != GCOL_TRANSFER_UNKNOWN;
}

/**
 * True when @p info can feed gcol_transform_create without an ICC LUT path.
 */
static bool gimg_color_info_matrix_ready(const GCOL_Color_Info * info) {
  return info && info->primaries_stated && info->white_stated &&
      info->transfer != GCOL_TRANSFER_UNKNOWN;
}

typedef struct {
  GCOL_ICC_Profile * icc;
  GCOL_Color_Info info;
  bool use_lut; ///< true: create_icc / create_to_icc with @a icc
  bool own_icc;
  bool gray;    ///< A gray description: matrix path, GCOL_PIXEL_GRAY.
} gimg_color_endpoint;

static void gimg_color_endpoint_clear(gimg_color_endpoint * e) {
  if (e->own_icc) {
    gcol_icc_free(e->icc);
  }
  e->icc = NULL;
  e->own_icc = false;
  e->use_lut = false;
  e->gray = false;
  memset(&e->info, 0, sizeof(e->info));
}

/**
 * An allocator for libs/color that goes through the library's own allocation
 * path.  libs/color is handed the allocator it is to use for the profile it
 * parses and the transform it builds; handing it the raster's own would let
 * those blocks bypass the memory scope of a load, a decode or a save, which
 * counts only what passes through gimg_malloc.  Everything made with this
 * allocator is destroyed before the operation returns, so it lives on the
 * operation's stack.
 */
typedef struct {
  const GIMG_Allocator * inner;
  GIMG_Allocator self;
} gimg_counted_allocator;

static void * gimg_counted_malloc(void * ctx, size_t n) {
  return gimg_malloc((const GIMG_Allocator *)ctx, n);
}

static void * gimg_counted_calloc(void * ctx, size_t n, size_t size) {
  return gimg_calloc((const GIMG_Allocator *)ctx, n, size);
}

static void * gimg_counted_realloc(void * ctx, void * p, size_t n) {
  return gimg_realloc((const GIMG_Allocator *)ctx, p, n);
}

static void gimg_counted_free(void * ctx, void * p) {
  gimg_free((const GIMG_Allocator *)ctx, p);
}

static void gimg_counted_allocator_init(
    gimg_counted_allocator * a, const GIMG_Allocator * inner) {
  a->inner = gimg_alloc_or_default(inner);
  a->self.ctx = (void *)a->inner;
  a->self.malloc_fn = gimg_counted_malloc;
  a->self.calloc_fn = gimg_counted_calloc;
  a->self.realloc_fn = gimg_counted_realloc;
  a->self.free_fn = gimg_counted_free;
}

/**
 * Resolve a Color_Info (and optional ICC bytes) into a transform endpoint.
 *
 * Matrix/TRC ICC - RGB or gray - becomes a description. LUT / CMYK ICC stays
 * as a profile. A description without ICC needs primaries, white and
 * transfer, or for gray a white and a transfer.
 */
static GIMG_Result gimg_color_endpoint_from_info(
    const GIMG_Allocator * allocator, const GCOL_Color_Info * in,
    bool is_cmyk_raster, gimg_color_endpoint * out) {
  memset(out, 0, sizeof(*out));
  if (!in) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (in->icc_bytes && in->icc_size > 0u) {
    GCOL_Result cr =
        gcol_icc_parse(allocator, NULL, in->icc_bytes, in->icc_size, &out->icc);
    if (cr != GCOL_OK) {
      return gimg_map_gcol_result(cr);
    }
    out->own_icc = true;
    if (gcol_icc_is_matrix_trc(out->icc)) {
      cr = gcol_icc_to_color_info(out->icc, &out->info);
      if (cr != GCOL_OK) {
        gimg_color_endpoint_clear(out);
        return gimg_map_gcol_result(cr);
      }
      /* Polarity (and any CMYK-only fields) stay with the caller's info. */
      out->info.cmyk_polarity = in->cmyk_polarity;
      out->use_lut = false;
      out->gray = gcol_icc_data_space(out->icc) == GIMG_ICC_SIG_GRAY;
      return GIMG_OK;
    }
    if (gcol_icc_data_space(out->icc) == GIMG_ICC_SIG_GRAY) {
      // A gray LUT profile: libs/color's LUT paths are RGB and CMYK.
      gimg_color_endpoint_clear(out);
      return GIMG_ERR_UNSUPPORTED;
    }
    out->use_lut = true;
    out->info = *in;
    out->info.icc_bytes = NULL;
    out->info.icc_size = 0;
    out->info.icc_linked_path = NULL;
    return GIMG_OK;
  }
  if (is_cmyk_raster) {
    /* Untagged CMYK has no safe default (no SWOP/FOGRA guess). */
    return GIMG_ERR_UNSUPPORTED;
  }
  out->gray = gimg_color_info_gray_ready(in);
  if (!out->gray && !gimg_color_info_matrix_ready(in)) {
    return GIMG_ERR_UNSUPPORTED;
  }
  out->info = *in;
  out->info.icc_bytes = NULL;
  out->info.icc_size = 0;
  out->info.icc_linked_path = NULL;
  out->use_lut = false;
  return GIMG_OK;
}

GIMG_API void gimg_color_transform_options_default(
    GIMG_Color_Transform_Options * options) {
  if (!options) {
    return;
  }
  memset(options, 0, sizeof(*options));
  gcol_color_info_default(&options->dest);
  options->intent = GCOL_INTENT_RELATIVE_COLORIMETRIC;
  options->flags = 0u;
}

GIMG_API void gimg_color_transform_options_srgb(
    GIMG_Color_Transform_Options * options) {
  gimg_color_transform_options_default(options);
  if (!options) {
    return;
  }
  (void)gcol_color_info_set_gamut(&options->dest, GCOL_PRIMARIES_SRGB);
  options->dest.transfer = GCOL_TRANSFER_SRGB;
  (void)gcol_transfer_conventions(GCOL_TRANSFER_SRGB, &options->dest.reference,
      &options->dest.sample_scale, &options->dest.white_luminance);
}

/** The storage a raster's samples have, as this transform reads them. */
typedef enum {
  GIMG_XF_U8 = 0, ///< One byte, 0..255.
  GIMG_XF_U12,    ///< uint16_t, 0..4095.
  GIMG_XF_U16,    ///< uint16_t, 0..65535.
  GIMG_XF_F32     ///< float, linear or encoded, unbounded.
} gimg_xf_storage;

/** Which model the raster carries, and in what storage, or false. */
static bool gimg_xf_classify(const GIMG_Pixel_Format * fmt,
    GCOL_Pixel_Model * model, unsigned * channels, gimg_xf_storage * storage) {
  if (!fmt || fmt->layout != GIMG_LAYOUT_INTERLEAVED) {
    return false;
  }
  if (fmt->channel_model == GIMG_CHANNEL_RGBA && fmt->channel_count == 4u) {
    *model = GCOL_PIXEL_RGB;
  }
  else if (fmt->channel_model == GIMG_CHANNEL_GRAY &&
      fmt->channel_count == 1u) {
    *model = GCOL_PIXEL_GRAY;
  }
  else if (fmt->channel_model == GIMG_CHANNEL_CMYK &&
      fmt->channel_count == 4u) {
    *model = GCOL_PIXEL_CMYK;
  }
  else {
    return false;
  }
  *channels = fmt->channel_count;
  const unsigned bits = gimg_pixel_format_channel_bits(fmt, 0);
  for (uint8_t i = 1; i < fmt->channel_count; i++) {
    if (gimg_pixel_format_channel_bits(fmt, i) != bits) {
      return false;
    }
  }
  if (fmt->channel_type == GIMG_CHANNEL_FLOAT) {
    // Float CMYK has no format here, and so no reader either.
    *storage = GIMG_XF_F32;
    return bits == 32u && *model != GCOL_PIXEL_CMYK;
  }
  if (fmt->channel_type != GIMG_CHANNEL_UNORM) {
    return false;
  }
  switch (bits) {
  case 8u:
    *storage = GIMG_XF_U8;
    return true;
  case 12u:
    *storage = GIMG_XF_U12;
    return true;
  case 16u:
    *storage = GIMG_XF_U16;
    return true;
  default:
    return false;
  }
}

/** The output format for @p model at the source's storage, or NULL. */
static const GIMG_Pixel_Format * gimg_xf_out_format(
    GCOL_Pixel_Model model, gimg_xf_storage storage) {
  static const GIMG_Pixel_Format * const table[3][4] = {
      {&GIMG_PIXEL_RGBA8, &GIMG_PIXEL_RGBA12, &GIMG_PIXEL_RGBA16,
          &GIMG_PIXEL_RGBAF32},
      {&GIMG_PIXEL_CMYK8, &GIMG_PIXEL_CMYK12, &GIMG_PIXEL_CMYK16, NULL},
      {&GIMG_PIXEL_GRAY8, &GIMG_PIXEL_GRAY12, &GIMG_PIXEL_GRAY16,
          &GIMG_PIXEL_GRAYF32},
  };
  const int row = model == GCOL_PIXEL_RGB ? 0
      : model == GCOL_PIXEL_CMYK          ? 1
                                          : 2;
  return table[row][storage];
}

static double gimg_xf_full(gimg_xf_storage storage) {
  return storage == GIMG_XF_U12 ? 4095.0
      : storage == GIMG_XF_U16  ? 65535.0
      : storage == GIMG_XF_U8   ? 255.0
                                : 1.0;
}

/** Sample @p i of a row of @p storage, as a unit-interval (or free) float. */
static float gimg_xf_read(
    const unsigned char * row, size_t i, gimg_xf_storage storage) {
  switch (storage) {
  case GIMG_XF_U8:
    return (float)row[i] / 255.0f;
  case GIMG_XF_U12:
  case GIMG_XF_U16: {
    uint16_t v;
    memcpy(&v, row + i * 2u, sizeof v);
    return (float)((double)v / gimg_xf_full(storage));
  }
  case GIMG_XF_F32:
  default: {
    float v;
    memcpy(&v, row + i * 4u, sizeof v);
    return v;
  }
  }
}

/** Store @p v as sample @p i of a row: rounded and clamped, or as is. */
static void gimg_xf_write(
    unsigned char * row, size_t i, gimg_xf_storage storage, float v) {
  if (storage == GIMG_XF_F32) {
    memcpy(row + i * 4u, &v, sizeof v);
    return;
  }
  double d = (double)v;
  d = !(d > 0.0) ? 0.0 : d > 1.0 ? 1.0 : d;
  const double q = d * gimg_xf_full(storage) + 0.5;
  if (storage == GIMG_XF_U8) {
    row[i] = (unsigned char)q;
    return;
  }
  const uint16_t w = (uint16_t)q;
  memcpy(row + i * 2u, &w, sizeof w);
}

static GIMG_Result ops_transform_color(const GIMG_Raster * src,
    const GIMG_Color_Transform_Options * options, GIMG_Raster ** out_raster) {
  if (out_raster) {
    *out_raster = NULL;
  }
  if (!src || !options || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }

  gimg_counted_allocator counted;
  gimg_counted_allocator_init(&counted, gimg_raster_allocator(src));

  const GIMG_Pixel_Format * fmt = gimg_raster_format(src);
  GCOL_Pixel_Model raster_model;
  unsigned raster_channels = 0;
  gimg_xf_storage storage;
  if (!gimg_xf_classify(fmt, &raster_model, &raster_channels, &storage)) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (fmt->alpha_form == GIMG_ALPHA_PREMULTIPLIED) {
    // A color transform acts on color, and premultiplied color is not it.
    return GIMG_ERR_UNSUPPORTED;
  }
  const bool src_cmyk = raster_model == GCOL_PIXEL_CMYK;

  const GCOL_Color_Info * src_ci = gimg_raster_color_info_const(src);
  GCOL_Color_Info src_fallback;
  if (!src_ci) {
    gcol_color_info_default(&src_fallback);
    src_ci = &src_fallback;
  }

  gimg_color_endpoint src_ep;
  gimg_color_endpoint dst_ep;
  memset(&src_ep, 0, sizeof(src_ep));
  memset(&dst_ep, 0, sizeof(dst_ep));

  GIMG_Result r =
      gimg_color_endpoint_from_info(
          &counted.self, src_ci, src_cmyk, &src_ep);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_color_endpoint_from_info(
      &counted.self, &options->dest, false, &dst_ep);
  if (r != GIMG_OK) {
    gimg_color_endpoint_clear(&src_ep);
    return r;
  }

#define GIMG_XF_REFUSE(code)                                                   \
  do {                                                                         \
    gimg_color_endpoint_clear(&src_ep);                                        \
    gimg_color_endpoint_clear(&dst_ep);                                        \
    return (code);                                                             \
  } while (0)

  /* Both LUT endpoints have no single color API - refuse. */
  if (src_ep.use_lut && dst_ep.use_lut) {
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED);
  }
  // What the source samples are fed to libs/color as.  A gray raster
  // described as RGB (a gray PNG with an sRGB chunk, say) is that RGB space's
  // neutrals, R = G = B.  The reverse - colour samples under a gray
  // description - says nothing about the colour, and is refused.
  GCOL_Pixel_Model feed_model = raster_model;
  if (raster_model == GCOL_PIXEL_GRAY && !src_ep.gray) {
    feed_model = GCOL_PIXEL_RGB;
  }
  if (src_ep.gray && raster_model != GCOL_PIXEL_GRAY) {
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED);
  }
  if (src_ep.gray && (dst_ep.use_lut)) {
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED); // No gray LUT path in libs/color.
  }
  if (src_ep.use_lut && dst_ep.gray) {
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED);
  }
  if (feed_model == GCOL_PIXEL_RGB && src_ep.use_lut &&
      gcol_icc_data_space(src_ep.icc) == GIMG_ICC_SIG_CMYK) {
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED);
  }

  GCOL_CMYK_Polarity polarity = src_ci->cmyk_polarity;
  if (src_cmyk && polarity == GCOL_CMYK_POLARITY_UNKNOWN) {
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED);
  }
  const bool dest_cmyk = dst_ep.use_lut &&
      gcol_icc_data_space(dst_ep.icc) == GIMG_ICC_SIG_CMYK;
  if (dest_cmyk &&
      options->dest.cmyk_polarity == GCOL_CMYK_POLARITY_UNKNOWN) {
    /* Writing CMYK needs a polarity the same way reading does. */
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED);
  }
  const GCOL_Pixel_Model out_model = dest_cmyk ? GCOL_PIXEL_CMYK
      : dst_ep.gray                            ? GCOL_PIXEL_GRAY
                                               : GCOL_PIXEL_RGB;
  const GIMG_Pixel_Format * out_fmt = gimg_xf_out_format(out_model, storage);
  if (!out_fmt) {
    GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED); // Float into CMYK.
  }
  const bool src_alpha = raster_model == GCOL_PIXEL_RGB;
  const uint32_t width = gimg_raster_width(src);
  const uint32_t height = gimg_raster_height(src);
  const unsigned char * src_px =
      (const unsigned char *)gimg_raster_pixels_const(src);
  const size_t src_stride = gimg_raster_stride_bytes(src);
  if (src_alpha && out_model != GCOL_PIXEL_RGB) {
    // A gray or CMYK result has nowhere to put alpha, so only an opaque
    // picture can go there without losing something it said.
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = src_px + (size_t)y * src_stride;
      for (uint32_t x = 0; x < width; x++) {
        if (gimg_xf_read(row, (size_t)x * 4u + 3u, storage) != 1.0f) {
          GIMG_XF_REFUSE(GIMG_ERR_UNSUPPORTED);
        }
      }
    }
  }

  GCOL_Transform * xform = NULL;
  GCOL_Result cr = GCOL_ERR_INTERNAL;
  if (src_ep.use_lut) {
    cr = gcol_transform_create_icc(&counted.self, src_ep.icc, &dst_ep.info,
        options->intent, options->flags, polarity, &xform);
  }
  else if (dst_ep.use_lut) {
    cr = gcol_transform_create_to_icc(&counted.self, &src_ep.info, dst_ep.icc,
        options->intent, options->flags, options->dest.cmyk_polarity, &xform);
  }
  else {
    cr = gcol_transform_create_model(&counted.self, &src_ep.info,
        src_ep.gray ? GCOL_PIXEL_GRAY : GCOL_PIXEL_RGB, &dst_ep.info,
        dst_ep.gray ? GCOL_PIXEL_GRAY : GCOL_PIXEL_RGB, options->intent,
        options->flags, &xform);
  }
  if (cr != GCOL_OK) {
    GIMG_XF_REFUSE(gimg_map_gcol_result(cr));
  }

  GIMG_Raster * out = NULL;
  r = gimg_raster_create_with_allocator(gimg_raster_allocator(src), width,
      height, out_fmt, GIMG_RASTER_OWNED, NULL, 0, &out);
  if (r != GIMG_OK) {
    gcol_transform_free(xform);
    GIMG_XF_REFUSE(r);
  }

  // Each row is handed to libs/color as it is stored when that is eight
  // bits - the path every 8-bit transform has always taken - and as floats
  // otherwise, which carry sixteen bits exactly and let a float raster keep
  // values past [0, 1].  The colour channels plus alpha when there is one;
  // a gray raster under an RGB description is widened to R = G = B first.
  const bool direct = storage == GIMG_XF_U8 && feed_model == raster_model;
  const unsigned feed_colour = feed_model == GCOL_PIXEL_GRAY ? 1u
      : feed_model == GCOL_PIXEL_CMYK                        ? 4u
                                                             : 3u;
  const unsigned feed_channels = feed_colour + (src_alpha ? 1u : 0u);
  const unsigned out_colour = out_model == GCOL_PIXEL_GRAY ? 1u
      : out_model == GCOL_PIXEL_CMYK                       ? 4u
                                                           : 3u;
  // Alpha goes through libs/color when both sides have it; an RGB result
  // from a source without one is made opaque below.
  const unsigned res_channels =
      out_colour + ((src_alpha && out_model == GCOL_PIXEL_RGB) ? 1u : 0u);
  const GCOL_Sample_Type type =
      storage == GIMG_XF_U8 ? GCOL_SAMPLE_UINT8 : GCOL_SAMPLE_FLOAT32;
  const size_t sample = storage == GIMG_XF_U8 ? 1u : sizeof(float);
  GCOL_Sample_Layout feed_layout = {type, feed_channels, feed_model};
  GCOL_Sample_Layout res_layout = {type, res_channels, out_model};

  const GIMG_Allocator * alloc = gimg_raster_allocator(src);
  unsigned char * feed = NULL;
  unsigned char * res = (unsigned char *)gimg_malloc(
      alloc, (size_t)width * res_channels * sample);
  if (!direct) {
    feed = (unsigned char *)gimg_malloc(
        alloc, (size_t)width * feed_channels * sample);
  }
  if (!res || (!direct && !feed)) {
    gimg_free(alloc, res);
    gimg_free(alloc, feed);
    gimg_raster_destroy(out);
    gcol_transform_free(xform);
    GIMG_XF_REFUSE(GIMG_ERR_OOM);
  }

  unsigned char * dst_px = (unsigned char *)gimg_raster_pixels(out);
  const size_t dst_stride = gimg_raster_stride_bytes(out);
  const unsigned out_channels = out_fmt->channel_count;
  for (uint32_t y = 0; y < height && cr == GCOL_OK; y++) {
    const unsigned char * srow = src_px + (size_t)y * src_stride;
    const unsigned char * in = srow;
    if (!direct) {
      for (uint32_t x = 0; x < width; x++) {
        for (unsigned c = 0; c < feed_channels; c++) {
          // Widening gray: every colour channel reads the one sample.  A
          // gray raster has no alpha, so there is nothing after it.
          const unsigned from = feed_model != raster_model ? 0u : c;
          const float v = gimg_xf_read(
              srow, (size_t)x * raster_channels + from, storage);
          if (storage == GIMG_XF_U8) {
            feed[(size_t)x * feed_channels + c] =
                (unsigned char)(v * 255.0f + 0.5f);
          }
          else {
            memcpy(feed + ((size_t)x * feed_channels + c) * sizeof(float), &v,
                sizeof v);
          }
        }
      }
      in = feed;
    }
    cr = gcol_transform_apply(
        xform, &feed_layout, in, &res_layout, res, (size_t)width);
    unsigned char * drow = dst_px + (size_t)y * dst_stride;
    for (uint32_t x = 0; x < width && cr == GCOL_OK; x++) {
      for (unsigned c = 0; c < out_channels; c++) {
        float v = 1.0f; // Alpha the result has and the source did not.
        if (c < res_channels) {
          const size_t at = (size_t)x * res_channels + c;
          if (storage == GIMG_XF_U8) {
            v = (float)res[at] / 255.0f;
          }
          else {
            memcpy(&v, res + at * sizeof(float), sizeof v);
          }
        }
        if (storage == GIMG_XF_U8 && c < res_channels) {
          drow[(size_t)x * out_channels + c] =
              res[(size_t)x * res_channels + c];
        }
        else {
          gimg_xf_write(drow, (size_t)x * out_channels + c, storage, v);
        }
      }
    }
  }

  gimg_free(alloc, res);
  gimg_free(alloc, feed);
  gcol_transform_free(xform);
  gimg_color_endpoint_clear(&src_ep);
  gimg_color_endpoint_clear(&dst_ep);
#undef GIMG_XF_REFUSE

  if (cr != GCOL_OK) {
    gimg_raster_destroy(out);
    return gimg_map_gcol_result(cr);
  }

  /* Result means what the caller asked for as dest. */
  r = gimg_raster_set_color_info(out, &options->dest);
  if (r != GIMG_OK) {
    gimg_raster_destroy(out);
    return r;
  }

  *out_raster = out;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_ops_transform_color(const GIMG_Raster * src,
    const GIMG_Color_Transform_Options * options, GIMG_Raster ** out_raster) {
  if (out_raster) {
    *out_raster = NULL;
  }
  if (!src || !options || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  gimg_limits_guard_t guard;
  gimg_limits_guard_open(&guard, options->limits);
  GIMG_Result r = GIMG_ERR_LIMIT;
  if (gimg_limits_guard_pixels_ok(
          &guard, gimg_raster_width(src), gimg_raster_height(src))) {
    r = ops_transform_color(src, options, out_raster);
  }
  return gimg_limits_guard_close(&guard, r, out_raster);
}
