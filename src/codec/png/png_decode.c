/**
 * @file
 *
 * PNG decode: DEFLATE decompress IDAT, apply filters, produce raster.
 *
 * Specification references:
 * - W3C PNG: https://www.w3.org/TR/PNG/ (Recommendation 10 Nov 2003)
 * - W3C Data representation (filtering, interlace):
 * https://www.w3.org/TR/PNG-DataRep.html
 * - W3C Filter algorithms: https://www.w3.org/TR/PNG-Filters.html
 * - ISO/IEC 15948:2004 (PNG — Portable Network Graphics)
 * - Interlaced data order (Adam7): W3C §2.6
 *   https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Pipeline: (1) Decompress concatenated IDAT payload (zlib: skip 2-byte header
 * and 4-byte Adler-32, feed middle bytes to DEFLATE). (2) Apply row filters in
 * reverse (None, Sub, Up, Average, Paeth per W3C PNG-Filters). (3) Convert
 * raw samples to raster (grayscale/palette/RGBA, 8/16-bit, scale 1/2/4-bit to
 * 8-bit). For interlaced (Adam7), step (2) is applied per pass; then we
 * reassemble into a single raster by writing each pass into the correct
 * pixel positions (x = x_offset + i*x_step, y = y_offset + j*y_step).
 *
 * Color chunk priority: PNG allows at most one of sRGB, iCCP, or gAMA (with
 * optional cHRM) for color interpretation. We use first in priority order:
 * sRGB > iCCP > gAMA. gimg_png_fill_color_info_from_ancillary() implements
 * this; iCCP payload is decompressed (DEFLATE) with a max size limit (bomb
 * protection) and the decompressed ICC bytes are attached to the raster.
 *
 * Limits: GIMG_Decode_Options.limits (max_decoded_pixels) is enforced before
 * allocating the raster. The compress library DEFLATE decoder is given
 * limits.max_output_bytes (from the same or default limits) to cap decompressed
 * IDAT size.
 *
 * APNG frame assembly: For multi-frame (APNG) we produce a full composited
 * canvas at IHDR dimensions. For each frame index we iterate from 0 to that
 * index: (1) Apply previous frame's dispose (BACKGROUND = clear rect to
 * transparent black; PREVIOUS = restore rect from a saved copy of the canvas
 * before that frame was drawn). (2) If current frame has PREVIOUS, save the
 * current canvas rect before drawing. (3) Decode this frame's raw pixels
 * (DEFLATE + filters), then blend onto canvas: SOURCE = replace rect;
 * OVER = alpha-blend. The returned raster is the canvas after the requested
 * frame. prev_rect is used only to restore for DISPOSE_PREVIOUS.
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../raster/raster_internal.h"
#include "../codec_internal.h"
#include "png_internal.h"

/**
 * Color chunk policy (PNG allows at most one of sRGB, iCCP, or gAMA+cHRM).
 * We use first in priority order: sRGB > iCCP > gAMA/cHRM.
 * Fills @a out_info; for iCCP allocates decompressed profile and sets
 * @a out_icc_owned (caller frees). Returns 1 if color info was set, 0 if none.
 */
static int gimg_png_fill_color_info_from_ancillary(
    const gimg_png_doc_state_t * state, const GIMG_Allocator * alloc,
    GIMG_Color_Info * out_info, void ** out_icc_owned, size_t * out_icc_size) {
  gimg_color_info_default(out_info);
  *out_icc_owned = NULL;
  *out_icc_size = 0;

  size_t first_srgb = (size_t)-1, first_iccp = (size_t)-1,
         first_gama = (size_t)-1;
  for (size_t i = 0; i < state->ancillary_count; i++) {
    gimg_png_chunk_type_t t = state->ancillary[i].type;
    if (t == GIMG_PNG_sRGB && first_srgb == (size_t)-1) {
      first_srgb = i;
    }
    else if (t == GIMG_PNG_iCCP && first_iccp == (size_t)-1) {
      first_iccp = i;
    }
    else if (t == GIMG_PNG_gAMA && first_gama == (size_t)-1) {
      first_gama = i;
    }
  }

  // Priority: sRGB > iCCP > gAMA.
  if (first_srgb != (size_t)-1) {
    const unsigned char * p = state->ancillary[first_srgb].payload;
    size_t len = state->ancillary[first_srgb].payload_size;
    if (len >= 1) {
      unsigned int intent = (unsigned int)p[0];
      if (intent > 3) {
        intent = 0;
      }
      out_info->primaries = GIMG_PRIMARIES_SRGB;
      out_info->white_point = GIMG_PRIMARIES_SRGB;
      out_info->transfer = GIMG_TRANSFER_SRGB;
      out_info->intent = (GIMG_Rendering_Intent)intent;
      return 1;
    }
  }
  if (first_iccp != (size_t)-1) {
    const unsigned char * payload = state->ancillary[first_iccp].payload;
    size_t payload_len = state->ancillary[first_iccp].payload_size;
    const unsigned char * name_end =
        (const unsigned char *)memchr(payload, 0, payload_len);
    if (name_end && name_end - payload + 2 < (ptrdiff_t)payload_len) {
      size_t name_len = (size_t)(name_end - payload);
      uint8_t comp = payload[name_len + 1];
      const unsigned char * zlib_start = payload + name_len + 2;
      size_t zlib_len = payload_len - name_len - 2;
      if (comp == 0 && zlib_len > 6) {
        const unsigned char * deflate_src = zlib_start + 2;
        size_t deflate_len = zlib_len - 6;
        size_t max_out = GIMG_PNG_ICC_MAX_DECODED;
        void * decoded = gimg_malloc(alloc, max_out);
        if (!decoded) {
          return 0;
        }
        size_t out_len = 0;
        gcomp_options_t * gopts = NULL;
        if (gimg_png_deflate_options_for_decode(max_out, &gopts) != GIMG_OK) {
          gimg_free(alloc, decoded);
          return 0;
        }
        gcomp_status_t gs = gcomp_decode_buffer(gcomp_registry_default(),
            "deflate", gopts, deflate_src, deflate_len, decoded, max_out,
            &out_len);
        gcomp_options_destroy(gopts);
        if (gs != GCOMP_OK) {
          gimg_free(alloc, decoded);
          return 0;
        }
        out_info->primaries = GIMG_PRIMARIES_UNKNOWN;
        out_info->white_point = GIMG_PRIMARIES_UNKNOWN;
        out_info->transfer = GIMG_TRANSFER_UNKNOWN;
        out_info->icc_bytes = decoded;
        out_info->icc_size = out_len;
        *out_icc_owned = decoded;
        *out_icc_size = out_len;
        return 1;
      }
    }
  }
  if (first_gama != (size_t)-1) {
    const unsigned char * p = state->ancillary[first_gama].payload;
    size_t len = state->ancillary[first_gama].payload_size;
    if (len >= 4) {
      uint32_t gama_val = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
          (uint32_t)p[2] << 8 | (uint32_t)p[3];
      if (gama_val > 0) {
        out_info->primaries = GIMG_PRIMARIES_UNKNOWN;
        out_info->white_point = GIMG_PRIMARIES_UNKNOWN;
        out_info->transfer = GIMG_TRANSFER_GAMMA;
        out_info->gamma_value =
            (double)gama_val / (double)GIMG_PNG_GAMA_SCALE;
        return 1;
      }
    }
  }
  return 0;
}

/** Blend frame rectangle onto canvas at (fx,fy). SOURCE = replace; OVER = alpha
 * blend. */
static void gimg_png_apng_blend_frame(unsigned char * canvas,
    size_t canvas_stride, const unsigned char * frame_pixels,
    size_t frame_stride, uint32_t fx, uint32_t fy, uint32_t fw, uint32_t fh,
    size_t bpp, int blend_over) {
  if (bpp == 1) {
    // Grayscale: no alpha; treat Over as replace.
    for (uint32_t y = 0; y < fh; y++) {
      unsigned char * dst =
          canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * bpp;
      const unsigned char * src = frame_pixels + (size_t)y * frame_stride;
      memcpy(dst, src, (size_t)fw * bpp);
    }
    return;
  }
  if (bpp == 2) {
    // Gray16: no alpha; replace.
    for (uint32_t y = 0; y < fh; y++) {
      unsigned char * dst =
          canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * bpp;
      const unsigned char * src = frame_pixels + (size_t)y * frame_stride;
      memcpy(dst, src, (size_t)fw * bpp);
    }
    return;
  }
  if (bpp == 4) {
    // RGBA8
    for (uint32_t y = 0; y < fh; y++) {
      unsigned char * dst =
          canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * 4u;
      const unsigned char * src = frame_pixels + (size_t)y * frame_stride;
      for (uint32_t x = 0; x < fw; x++) {
        unsigned int sa = (unsigned int)src[3];
        if (!blend_over || sa == 255) {
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
          dst[3] = src[3];
        }
        else if (sa != 0) {
          unsigned int inv_sa = 255 - sa;
          dst[0] = (unsigned char)((src[0] * sa + dst[0] * inv_sa) / 255);
          dst[1] = (unsigned char)((src[1] * sa + dst[1] * inv_sa) / 255);
          dst[2] = (unsigned char)((src[2] * sa + dst[2] * inv_sa) / 255);
          dst[3] = (unsigned char)(sa + (unsigned int)dst[3] * inv_sa / 255);
        }
        src += 4;
        dst += 4;
      }
    }
    return;
  }
  if (bpp == 8) {
    // RGBA16: Porter-Duff Over (non-premultiplied). Per APNG / compositing:
    //   Co = (Cs*As + Cd*Ad*(1-As))/(Ao), Ao = As + Ad*(1-As).
    // For opaque destination (Ad=65535): Ao=65535, Co = (Cs*As + Cd*(65535-As))/65535.
    // 64-bit intermediates to avoid overflow (sc*sa + dc*inv_sa can exceed 2^32).
    for (uint32_t y = 0; y < fh; y++) {
      unsigned char * dst =
          canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * 8u;
      const unsigned char * src = frame_pixels + (size_t)y * frame_stride;
      if (!blend_over) {
        memcpy(dst, src, (size_t)fw * 8u);
      }
      else {
        for (uint32_t x = 0; x < fw; x++) {
          uint16_t sa = (uint16_t)src[6] | ((uint16_t)src[7] << 8);
          if (sa == 65535) {
            memcpy(dst, src, 8);
          }
          else if (sa != 0) {
            uint32_t inv_sa = 65535u - (uint32_t)sa;
            for (int c = 0; c < 4; c++) {
              uint16_t sc =
                  (uint16_t)src[c * 2] | ((uint16_t)src[c * 2 + 1] << 8);
              uint16_t dc =
                  (uint16_t)dst[c * 2] | ((uint16_t)dst[c * 2 + 1] << 8);
              uint64_t sum;
              if (c < 3) {
                sum = (uint64_t)sc * (uint32_t)sa + (uint64_t)dc * inv_sa;
              }
              else {
                sum = (uint64_t)sa * 65535u + (uint64_t)dc * inv_sa;
              }
              uint32_t v = (uint32_t)(sum / 65535u);
              if (v > 65535u) {
                v = 65535u;
              }
              dst[c * 2] = (unsigned char)(v & 0xFFu);
              dst[c * 2 + 1] = (unsigned char)(v >> 8);
            }
          }
          src += 8;
          dst += 8;
        }
      }
    }
  }
}

GIMG_Result gimg_png_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!codec || !item || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  const GIMG_Doc * doc = item->doc;
  if (!doc || !doc->codec_private) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // Allow decode when the codec that loaded the doc is the same as the one
  // requesting decode (pointer match), or when they match by name (handles
  // save path where codec comes from gimg_codec_by_name and may differ by
  // pointer from doc->loaded_by_codec in some link scenarios).
  const GIMG_Codec * doc_codec = doc->loaded_by_codec;
  if (!doc_codec ||
      (doc_codec != codec &&
          (!gimg_codec_name(doc_codec) || !gimg_codec_name(codec) ||
              strcmp(gimg_codec_name(doc_codec), gimg_codec_name(codec)) !=
                  0))) {
    return GIMG_ERR_UNSUPPORTED;
  }

  gimg_png_doc_state_t * state = (gimg_png_doc_state_t *)doc->codec_private;

  const gimg_png_ihdr_t * ihdr = &state->ihdr;
  uint32_t frame_width = ihdr->width;
  uint32_t frame_height = ihdr->height;
  const unsigned char * idat_ptr = state->idat;
  size_t idat_len = state->idat_size;

  if (state->is_apng && state->frames) {
    if (item->index >= state->frame_count) {
      return GIMG_ERR_UNSUPPORTED;
    }
    // APNG: return full composited canvas (IHDR dimensions) with dispose/blend
    // applied.
    {
      const GIMG_Allocator * a = gimg_alloc_or_default(codec->allocator);
      const GIMG_Limits * lim = options ? options->limits : NULL;
      uint32_t canvas_w = ihdr->width;
      uint32_t canvas_h = ihdr->height;
      size_t canvas_pixels = (size_t)canvas_w * (size_t)canvas_h;
      size_t max_px = (lim && lim->max_decoded_pixels != 0)
          ? lim->max_decoded_pixels
          : canvas_pixels;
      if (canvas_pixels > max_px) {
        return GIMG_ERR_LIMIT;
      }
      int use_trns = (state->trns && state->trns_size > 0) ? 1 : 0;
      const GIMG_Pixel_Format * fmt = NULL;
      switch (ihdr->color_type) {
      case 0:
        fmt = (ihdr->bit_depth <= 8)
            ? (use_trns ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_GRAY8)
            : (use_trns ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_GRAY16);
        break;
      case 2:
        fmt = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
        break;
      case 3:
        fmt = &GIMG_PIXEL_RGBA8;
        break;
      case 4:
        fmt = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
        break;
      case 6:
        fmt = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
        break;
      default:
        return GIMG_ERR_FORMAT;
      }
      size_t bpp_out = gimg_raster_bytes_per_pixel(fmt);
      size_t canvas_stride = (size_t)canvas_w * bpp_out;
      if (canvas_stride % GIMG_DEFAULT_STRIDE_ALIGNMENT) {
        canvas_stride = (canvas_stride + GIMG_DEFAULT_STRIDE_ALIGNMENT - 1) &
            ~(size_t)(GIMG_DEFAULT_STRIDE_ALIGNMENT - 1);
      }
      unsigned char * canvas =
          (unsigned char *)gimg_malloc(a, canvas_stride * (size_t)canvas_h);
      if (!canvas) {
        return GIMG_ERR_OOM;
      }
      memset(canvas, 0, canvas_stride * (size_t)canvas_h);
      // prev_rect: for DISPOSE_PREVIOUS we save the frame rect before drawing.
      unsigned char * prev_rect =
          (unsigned char *)gimg_malloc(a, canvas_stride * (size_t)canvas_h);
      if (!prev_rect) {
        gimg_free(a, canvas);
        return GIMG_ERR_OOM;
      }
      memset(prev_rect, 0, canvas_stride * (size_t)canvas_h);
      for (size_t i = 0; i <= item->index; i++) {
        const gimg_png_fctl_t * fctl = &state->frames[i].fctl;
        uint32_t fx = fctl->x_offset;
        uint32_t fy = fctl->y_offset;
        uint32_t fw = fctl->width;
        uint32_t fh = fctl->height;
        if (i > 0) {
          const gimg_png_fctl_t * prev_fctl = &state->frames[i - 1].fctl;
          uint32_t px = prev_fctl->x_offset;
          uint32_t py = prev_fctl->y_offset;
          uint32_t pw = prev_fctl->width;
          uint32_t ph = prev_fctl->height;
          if (prev_fctl->dispose_op == 1) {
            // BACKGROUND: clear previous frame rect to transparent black.
            for (uint32_t y = 0; y < ph; y++) {
              unsigned char * row = canvas + (size_t)(py + y) * canvas_stride +
                  (size_t)px * bpp_out;
              memset(row, 0, (size_t)pw * bpp_out);
            }
          }
          else if (prev_fctl->dispose_op == 2) {
            // PREVIOUS: restore previous frame rect from prev_rect.
            for (uint32_t y = 0; y < ph; y++) {
              unsigned char * dst = canvas + (size_t)(py + y) * canvas_stride +
                  (size_t)px * bpp_out;
              const unsigned char * src = prev_rect +
                  (size_t)(py + y) * canvas_stride + (size_t)px * bpp_out;
              memcpy(dst, src, (size_t)pw * bpp_out);
            }
          }
        }
        if (fctl->dispose_op == 2) {
          // Save current canvas content at this frame's rect before we draw.
          for (uint32_t y = 0; y < fh; y++) {
            const unsigned char * src = canvas +
                (size_t)(fy + y) * canvas_stride + (size_t)fx * bpp_out;
            unsigned char * dst = prev_rect + (size_t)(fy + y) * canvas_stride +
                (size_t)fx * bpp_out;
            memcpy(dst, src, (size_t)fw * bpp_out);
          }
        }
        void * frame_pixels = NULL;
        size_t frame_stride = 0;
        uint32_t dec_fw = 0, dec_fh = 0;
        GIMG_Result dr = gimg_png_decode_one_apng_frame(state, ihdr, i, fmt, a,
            lim, &frame_pixels, &frame_stride, &dec_fw, &dec_fh);
        if (dr != GIMG_OK) {
          gimg_free(a, prev_rect);
          gimg_free(a, canvas);
          return dr;
        }
        gimg_png_apng_blend_frame(canvas, canvas_stride,
            (const unsigned char *)frame_pixels, frame_stride, fx, fy, dec_fw,
            dec_fh, bpp_out, (state->frames[i].fctl.blend_op == 1) ? 1 : 0);
        gimg_free(a, frame_pixels);
      }
      gimg_free(a, prev_rect);
      GIMG_Result rr = gimg_raster_create_with_allocator(a, canvas_w, canvas_h,
          fmt, GIMG_RASTER_OWNED, canvas, canvas_stride, out_raster);
      if (rr != GIMG_OK) {
        gimg_free(a, canvas);
        return rr;
      }
      {
        GIMG_Color_Info color_info;
        void * icc_owned = NULL;
        size_t icc_size = 0;
        if (gimg_png_fill_color_info_from_ancillary(
                state, a, &color_info, &icc_owned, &icc_size)) {
          gimg_raster_set_color_info(*out_raster, &color_info);
          if (icc_owned) {
            gimg_free(a, icc_owned);
          }
        }
      }
      return GIMG_OK;
    }
  }
  else if (item->index >= 1) {
    return GIMG_ERR_UNSUPPORTED;  // Single-frame only for non-APNG.
  }

  if (ihdr->interlace_method > 1) {
    return GIMG_ERR_UNSUPPORTED;
  }

  if (!idat_ptr) {
    return GIMG_ERR_FORMAT;
  }
  const GIMG_Allocator * alloc = codec->allocator;
  alloc = gimg_alloc_or_default(alloc);
  const GIMG_Limits * limits = options ? options->limits : NULL;

  // Select output format and whether we need alpha from tRNS.
  int use_trns = (state->trns && state->trns_size > 0) ? 1 : 0;
  const GIMG_Pixel_Format * format = NULL;
  switch (ihdr->color_type) {
  case 0:
    if (ihdr->bit_depth <= 8) {
      format = use_trns ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_GRAY8;
    }
    else {
      format = use_trns ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_GRAY16;
    }
    break;
  case 2:
    format = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
    break;
  case 3:
    format = &GIMG_PIXEL_RGBA8; // Palette always expanded to RGBA8 (+ tRNS).
    break;
  case 4:
    format = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
    break;
  case 6:
    format = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
    break;
  default:
    return GIMG_ERR_FORMAT;
  }
  if (ihdr->color_type == 3 && (!state->plte || state->plte_size == 0)) {
    return GIMG_ERR_FORMAT;
  }

  void * pixels = NULL;
  size_t stride = 0;
  GIMG_Result r = gimg_png_decode_idat_to_pixels(state, ihdr, idat_ptr,
      idat_len, frame_width, frame_height, format, alloc, limits, &pixels,
      &stride);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_raster_create_with_allocator(alloc, frame_width,
      frame_height, format, GIMG_RASTER_OWNED, pixels, stride, out_raster);
  if (r != GIMG_OK) {
    gimg_free(alloc, pixels);
    return r;
  }

  // Apply color chunks (sRGB > iCCP > gAMA/cHRM); store in raster color_info.
  {
    GIMG_Color_Info color_info;
    void * icc_owned = NULL;
    size_t icc_size = 0;
    if (gimg_png_fill_color_info_from_ancillary(
            state, alloc, &color_info, &icc_owned, &icc_size)) {
      r = gimg_raster_set_color_info(*out_raster, &color_info);
      if (icc_owned) {
        gimg_free(alloc, icc_owned);
      }
      if (r != GIMG_OK) {
        gimg_raster_destroy(*out_raster);
        *out_raster = NULL;
        return r;
      }
    }
  }

  return GIMG_OK;
}
