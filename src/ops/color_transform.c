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
#include "../raster/raster_internal.h"
#include "ops_internal.h"

/** ICC data-space signatures from the profile header (four-character codes). */
#define GIMG_ICC_SIG_RGB 0x52474220u  /* 'RGB ' */
#define GIMG_ICC_SIG_CMYK 0x434D594Bu /* 'CMYK' */

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
} gimg_color_endpoint;

static void gimg_color_endpoint_clear(gimg_color_endpoint * e) {
  if (e->own_icc) {
    gcol_icc_free(e->icc);
  }
  e->icc = NULL;
  e->own_icc = false;
  e->use_lut = false;
  memset(&e->info, 0, sizeof(e->info));
}

/**
 * Resolve a Color_Info (and optional ICC bytes) into a transform endpoint.
 *
 * Matrix/TRC ICC becomes a description. LUT / CMYK ICC stays as a profile.
 * A description without ICC needs primaries, white and transfer.
 */
static GIMG_Result gimg_color_endpoint_from_info(const GCOL_Color_Info * in,
    bool is_cmyk_raster, gimg_color_endpoint * out) {
  memset(out, 0, sizeof(*out));
  if (!in) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (in->icc_bytes && in->icc_size > 0u) {
    GCOL_Result cr =
        gcol_icc_parse(NULL, NULL, in->icc_bytes, in->icc_size, &out->icc);
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
      return GIMG_OK;
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
  if (!gimg_color_info_matrix_ready(in)) {
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

static bool gimg_format_is_rgba8(const GIMG_Pixel_Format * fmt) {
  return fmt && fmt->channel_model == GIMG_CHANNEL_RGBA &&
      fmt->channel_count == 4u && fmt->bits_per_channel[0] == 8u &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED;
}

static bool gimg_format_is_cmyk8(const GIMG_Pixel_Format * fmt) {
  return fmt && fmt->channel_model == GIMG_CHANNEL_CMYK &&
      fmt->channel_count == 4u && fmt->bits_per_channel[0] == 8u &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED;
}

static bool gimg_dest_is_cmyk(const gimg_color_endpoint * dest) {
  if (dest->use_lut && dest->icc) {
    return gcol_icc_data_space(dest->icc) == GIMG_ICC_SIG_CMYK;
  }
  return false;
}

GIMG_API GIMG_Result gimg_ops_transform_color(const GIMG_Raster * src,
    const GIMG_Color_Transform_Options * options, GIMG_Raster ** out_raster) {
  if (out_raster) {
    *out_raster = NULL;
  }
  if (!src || !options || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }

  const GIMG_Pixel_Format * fmt = gimg_raster_format(src);
  const bool src_rgba = gimg_format_is_rgba8(fmt);
  const bool src_cmyk = gimg_format_is_cmyk8(fmt);
  if (!src_rgba && !src_cmyk) {
    return GIMG_ERR_UNSUPPORTED;
  }
  for (uint8_t i = 1; i < fmt->channel_count && i < 8u; i++) {
    if (fmt->bits_per_channel[i] != fmt->bits_per_channel[0]) {
      return GIMG_ERR_UNSUPPORTED;
    }
  }

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
      gimg_color_endpoint_from_info(src_ci, src_cmyk, &src_ep);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_color_endpoint_from_info(&options->dest, false, &dst_ep);
  if (r != GIMG_OK) {
    gimg_color_endpoint_clear(&src_ep);
    return r;
  }

  /* Dest that is a CMYK LUT while the caller's dest Color_Info said RGB-only
   * is fine; dest that is matrix RGB while source is CMYK needs the LUT path
   * on the source. Both LUT endpoints have no single color API — refuse. */
  if (src_ep.use_lut && dst_ep.use_lut) {
    gimg_color_endpoint_clear(&src_ep);
    gimg_color_endpoint_clear(&dst_ep);
    return GIMG_ERR_UNSUPPORTED;
  }

  GCOL_CMYK_Polarity polarity = src_ci->cmyk_polarity;
  if (src_cmyk && polarity == GCOL_CMYK_POLARITY_UNKNOWN) {
    gimg_color_endpoint_clear(&src_ep);
    gimg_color_endpoint_clear(&dst_ep);
    return GIMG_ERR_UNSUPPORTED;
  }
  if (gimg_dest_is_cmyk(&dst_ep) &&
      options->dest.cmyk_polarity == GCOL_CMYK_POLARITY_UNKNOWN) {
    /* Writing CMYK needs a polarity the same way reading does. */
    gimg_color_endpoint_clear(&src_ep);
    gimg_color_endpoint_clear(&dst_ep);
    return GIMG_ERR_UNSUPPORTED;
  }

  GCOL_Transform * xform = NULL;
  GCOL_Result cr = GCOL_ERR_INTERNAL;
  if (src_ep.use_lut) {
    cr = gcol_transform_create_icc(NULL, src_ep.icc, &dst_ep.info,
        options->intent, options->flags, polarity, &xform);
  }
  else if (dst_ep.use_lut) {
    cr = gcol_transform_create_to_icc(NULL, &src_ep.info, dst_ep.icc,
        options->intent, options->flags, options->dest.cmyk_polarity, &xform);
  }
  else {
    cr = gcol_transform_create(NULL, &src_ep.info, &dst_ep.info, options->intent,
        options->flags, &xform);
  }
  if (cr != GCOL_OK) {
    gimg_color_endpoint_clear(&src_ep);
    gimg_color_endpoint_clear(&dst_ep);
    return gimg_map_gcol_result(cr);
  }

  const bool dest_cmyk = gimg_dest_is_cmyk(&dst_ep);
  const GIMG_Pixel_Format * out_fmt =
      dest_cmyk ? &GIMG_PIXEL_CMYK8 : &GIMG_PIXEL_RGBA8;
  const uint32_t width = gimg_raster_width(src);
  const uint32_t height = gimg_raster_height(src);
  const size_t pixel_count = (size_t)width * (size_t)height;

  GIMG_Raster * out = NULL;
  r = gimg_raster_create(width, height, out_fmt, GIMG_RASTER_OWNED, NULL, 0,
      &out);
  if (r != GIMG_OK) {
    gcol_transform_free(xform);
    gimg_color_endpoint_clear(&src_ep);
    gimg_color_endpoint_clear(&dst_ep);
    return r;
  }

  const unsigned char * src_px =
      (const unsigned char *)gimg_raster_pixels_const(src);
  unsigned char * dst_px = (unsigned char *)gimg_raster_pixels(out);

  GCOL_Sample_Layout src_layout;
  GCOL_Sample_Layout dst_layout;
  memset(&src_layout, 0, sizeof(src_layout));
  memset(&dst_layout, 0, sizeof(dst_layout));
  src_layout.type = GCOL_SAMPLE_UINT8;
  dst_layout.type = GCOL_SAMPLE_UINT8;

  if (src_cmyk) {
    src_layout.model = GCOL_PIXEL_CMYK;
    src_layout.channel_count = 4u;
  }
  else {
    src_layout.model = GCOL_PIXEL_RGB;
    src_layout.channel_count = 4u;
  }

  if (dest_cmyk) {
    dst_layout.model = GCOL_PIXEL_CMYK;
    dst_layout.channel_count = 4u;
    cr = gcol_transform_apply(
        xform, &src_layout, src_px, &dst_layout, dst_px, pixel_count);
  }
  else if (src_cmyk) {
    /* Apply into tightly packed RGB, then expand to opaque RGBA. */
    unsigned char * rgb =
        (unsigned char *)gimg_malloc(gimg_raster_allocator(src),
            pixel_count * 3u);
    if (!rgb) {
      gimg_raster_destroy(out);
      gcol_transform_free(xform);
      gimg_color_endpoint_clear(&src_ep);
      gimg_color_endpoint_clear(&dst_ep);
      return GIMG_ERR_OOM;
    }
    dst_layout.model = GCOL_PIXEL_RGB;
    dst_layout.channel_count = 3u;
    cr = gcol_transform_apply(
        xform, &src_layout, src_px, &dst_layout, rgb, pixel_count);
    if (cr == GCOL_OK) {
      for (size_t i = 0; i < pixel_count; i++) {
        dst_px[i * 4u + 0u] = rgb[i * 3u + 0u];
        dst_px[i * 4u + 1u] = rgb[i * 3u + 1u];
        dst_px[i * 4u + 2u] = rgb[i * 3u + 2u];
        dst_px[i * 4u + 3u] = 255u;
      }
    }
    gimg_free(gimg_raster_allocator(src), rgb);
  }
  else {
    dst_layout.model = GCOL_PIXEL_RGB;
    dst_layout.channel_count = 4u;
    cr = gcol_transform_apply(
        xform, &src_layout, src_px, &dst_layout, dst_px, pixel_count);
  }

  gcol_transform_free(xform);
  gimg_color_endpoint_clear(&src_ep);
  gimg_color_endpoint_clear(&dst_ep);

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
