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

#include <ghoti.io/image/macros.h>
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
 * @a out_icc_owned (caller frees). Returns true if color info was set, false if none.
 */
/**
 * @name cHRM
 *
 * cHRM (PNG 11.3.2.1) states the white point and the three primaries as x,y
 * chromaticities, each stored as the value times 100000.  It says nothing
 * about the transfer function, which is gAMA's job; the two are a pair, and a
 * file carrying gAMA alone leaves its gamut to the reader's assumption -
 * which is sRGB's, and is what the writer here relies on when it omits cHRM.
 *
 * The payload is carried through exactly.  GIMG_Color_Info stores
 * coordinates rather than a name, so a gamut this library has no name for is
 * still the gamut the file stated; gimg_gamut_identify() is what puts a name
 * to the ones it recognises, and answering "no name" there costs the caller
 * nothing it was given.
 * @{
 */

/**
 * Read a cHRM payload into a gamut.
 *
 * cHRM states all four points, so the result is exact and needs no table.
 * Naming it is gimg_gamut_identify()'s job and is a separate question from
 * carrying it: a gamut with no name here still reaches the caller intact.
 */
static bool gimg_png_gamut_from_chrm(
    const unsigned char * p, size_t len, GIMG_Gamut * out_gamut) {
  if (!p || len < GIMG_PNG_cHRM_LEN) {
    return false;
  }
  double value[8];
  for (unsigned int i = 0; i < 8; i++) {
    uint32_t raw = ((uint32_t)p[i * 4] << 24) |
        ((uint32_t)p[(i * 4) + 1] << 16) | ((uint32_t)p[(i * 4) + 2] << 8) |
        (uint32_t)p[(i * 4) + 3];
    value[i] = (double)raw / GIMG_PNG_cHRM_SCALE;
  }
  out_gamut->white.x = value[0];
  out_gamut->white.y = value[1];
  out_gamut->red.x = value[2];
  out_gamut->red.y = value[3];
  out_gamut->green.x = value[4];
  out_gamut->green.y = value[5];
  out_gamut->blue.x = value[6];
  out_gamut->blue.y = value[7];
  return true;
}
/** @} */

static bool gimg_png_fill_color_info_from_ancillary(
    const gimg_png_doc_state_t * state, const GIMG_Allocator * alloc,
    GIMG_Color_Info * out_info, void ** out_icc_owned, size_t * out_icc_size) {
  gimg_color_info_default(out_info);
  *out_icc_owned = NULL;
  *out_icc_size = 0;

  size_t first_srgb = (size_t)-1, first_iccp = (size_t)-1,
         first_gama = (size_t)-1, first_cicp = (size_t)-1,
         first_chrm = (size_t)-1;
  for (size_t i = 0; i < state->ancillary_count; i++) {
    gimg_png_chunk_type_t t = state->ancillary[i].type;
    if (t == GIMG_PNG_cICP && first_cicp == (size_t)-1) {
      first_cicp = i;
    }
    else if (t == GIMG_PNG_sRGB && first_srgb == (size_t)-1) {
      first_srgb = i;
    }
    else if (t == GIMG_PNG_iCCP && first_iccp == (size_t)-1) {
      first_iccp = i;
    }
    else if (t == GIMG_PNG_cHRM && first_chrm == (size_t)-1) {
      first_chrm = i;
    }
    else if (t == GIMG_PNG_gAMA && first_gama == (size_t)-1) {
      first_gama = i;
    }
  }

  // PNG Third Edition adds cICP and puts it ahead of everything else: when a
  // frame carries coding-independent code points, they say what the samples
  // mean and the other color chunks do not get a say.
  //
  // GIMG_Color_Info describes sRGB, Adobe RGB, linear and a plain gamma, and
  // CICP names a great deal more than that - BT.2020 primaries, PQ and HLG
  // transfer, limited-range signaling. Only the combination this model can
  // actually hold is translated; any other is left unknown rather than
  // rounded to the nearest thing we can say, which would be a claim about the
  // pixels that the file did not make. The chunk itself is kept either way,
  // so nothing is lost on the way through.
  if (first_cicp != (size_t)-1) {
    const unsigned char * p = state->ancillary[first_cicp].payload;
    size_t len = state->ancillary[first_cicp].payload_size;
    if (len >= GIMG_PNG_cICP_LEN) {
      unsigned int primaries = p[0];
      unsigned int transfer = p[1];
      unsigned int matrix = p[2];
      unsigned int full_range = p[3];
      // H.273 code points: primaries 1 and transfer 13 are the sRGB pair;
      // matrix 0 (identity) and full range are what PNG 3rd ed. requires of
      // an RGB image.
      if (primaries == 1u && transfer == 13u && matrix == 0u &&
          full_range == 1u) {
        (void)gimg_color_info_set_gamut(out_info, GIMG_PRIMARIES_SRGB);
        out_info->transfer = GIMG_TRANSFER_SRGB;
        return true;
      }
      // Transfer 8 is linear, and primaries 1 still names the sRGB gamut.
      if (primaries == 1u && transfer == 8u && matrix == 0u &&
          full_range == 1u) {
        (void)gimg_color_info_set_gamut(out_info, GIMG_PRIMARIES_SRGB);
        out_info->transfer = GIMG_TRANSFER_LINEAR;
        return true;
      }
      return false; // Understood, representable by nothing here.
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
      (void)gimg_color_info_set_gamut(out_info, GIMG_PRIMARIES_SRGB);
      out_info->transfer = GIMG_TRANSFER_SRGB;
      out_info->intent = (GIMG_Rendering_Intent)intent;
      return true;
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
        size_t max_out = GIMG_PNG_ICC_MAX_DECODED;
        void * decoded = gimg_malloc(alloc, max_out);
        if (!decoded) {
          return false;
        }
        size_t out_len = 0;
        // PNG 11.3.2.3: the profile is a zlib stream like any other, so the
        // same wrapper and Adler-32 checks apply.
        if (gimg_png_zlib_decode(zlib_start, zlib_len,
                (unsigned char *)decoded, max_out, &out_len) != GIMG_OK) {
          gimg_free(alloc, decoded);
          return false;
        }
        out_info->gamut_stated = false;
        out_info->transfer = GIMG_TRANSFER_UNKNOWN;
        out_info->icc_bytes = decoded;
        out_info->icc_size = out_len;
        *out_icc_owned = decoded;
        *out_icc_size = out_len;
        return true;
      }
    }
  }
  // gAMA and cHRM are a pair rather than alternatives: one states the
  // transfer and the other the gamut, and a file may carry either or both.
  // Read whichever is there and say nothing about the half that is absent - a
  // gamma with no cHRM leaves the gamut to the reader's assumption, which is
  // what the writer here relies on when it omits cHRM for an sRGB gamut.
  GIMG_Gamut gamut;
  bool have_gamut = false;
  if (first_chrm != (size_t)-1) {
    have_gamut = gimg_png_gamut_from_chrm(state->ancillary[first_chrm].payload,
        state->ancillary[first_chrm].payload_size, &gamut);
  }
  bool said_something = false;
  if (first_gama != (size_t)-1) {
    const unsigned char * p = state->ancillary[first_gama].payload;
    size_t len = state->ancillary[first_gama].payload_size;
    if (len >= 4) {
      uint32_t gama_val = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
          (uint32_t)p[2] << 8 | (uint32_t)p[3];
      if (gama_val > 0) {
        out_info->transfer = GIMG_TRANSFER_GAMMA;
        out_info->gamma_value =
            (double)gama_val / (double)GIMG_PNG_GAMA_SCALE;
        said_something = true;
      }
    }
  }
  if (have_gamut) {
    out_info->gamut = gamut;
    out_info->gamut_stated = true;
    said_something = true;
  }
  return said_something;
}

/** APNG frame compositing: blend frame rectangle onto canvas at (fx,fy).
 * SOURCE (blend_op 0): replace canvas region with frame pixels.
 * OVER (blend_op 1): Porter-Duff over operator (non-premultiplied). RGBA8 uses
 * 8-bit alpha; RGBA16 uses 64-bit intermediates (sc*sa + dc*inv_sa) and
 * divides by 65535 to avoid overflow. Grayscale/gray+alpha without alpha
 * channel are treated as replace. Order: caller must apply dispose (NONE/
 * BACKGROUND/PREVIOUS) to the canvas before calling this; then this blends
 * the decoded frame. See APNG spec and formats/png.md. */
static void gimg_png_apng_blend_frame(unsigned char * canvas,
    size_t canvas_stride, uint32_t canvas_w, uint32_t canvas_h,
    const unsigned char * frame_pixels, size_t frame_stride, uint32_t fx,
    uint32_t fy, uint32_t fw, uint32_t fh, size_t bpp, int blend_over) {
  // The loader refuses a frame that does not fit the canvas, so this should
  // never have anything to clip. It clips anyway: every loop below writes to
  // canvas + (fy + y) * stride + fx * bpp, so a frame rectangle that reaches
  // past the edge is a heap write past the end of the canvas - which is what
  // this used to do, for any file that declared one.
  //
  // The first sentence is a claim about another function, so it was checked
  // rather than assumed: png_apng_frame_outside_canvas.png and
  // png_apng_frame_zero_size.png both come back GIMG_ERR_FORMAT from the
  // load, while png_apng_frame_inside_canvas.png loads and decodes both its
  // frames. That is why these three tests never fire, and it is also why they
  // stay - they are what stands between a future loosening there and a heap
  // overflow here.
  if (fx >= canvas_w || fy >= canvas_h) {
    return;
  }
  if (fw > canvas_w - fx) {
    fw = canvas_w - fx;
  }
  if (fh > canvas_h - fy) {
    fh = canvas_h - fy;
  }
  if (bpp == 1 || bpp == 2) {
    // Grayscale at 8 or 16 bits.  Neither carries alpha, so BLEND_OP_OVER has
    // nothing to blend and both replace; the two were written out separately
    // and were identical line for line, which only gave them somewhere to
    // drift apart.  The `bpp` multiplier is what distinguishes them.
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
          // Porter-Duff Over, non-premultiplied (PNG "Alpha Channel
          // Processing", which APNG's BLEND_OP_OVER refers to):
          //   Ao = As + Ad*(1-As)
          //   Co = (Cs*As + Cd*Ad*(1-As)) / Ao
          // The destination's own alpha weights its colour, and the result is
          // divided by the composite alpha.  Dropping both - which is what
          // this did - is only correct when the destination is opaque, and
          // the error is not subtle: over a fully transparent pixel the
          // spec says the destination contributes nothing, while the
          // simplified form mixes in the colour stored behind its zero
          // alpha.  A canvas is transparent wherever no frame has painted
          // yet, so that is the common case, not a corner.
          //
          // Scaled to integers: with As = sa/255, Ad = da/255 and
          // (1-As) = inv_sa/255, both sides carry 255^2, leaving
          //   Co = (Cs*sa*255 + Cd*da*inv_sa) / (sa*255 + da*inv_sa)
          //   Ao = (sa*255 + da*inv_sa) / 255
          // The widest intermediate is 255*255*255, well inside uint32.
          const unsigned int inv_sa = 255u - sa;
          const unsigned int da = (unsigned int)dst[3];
          const unsigned int ao = sa * 255u + da * inv_sa; // never 0: sa > 0
          dst[0] = (unsigned char)(
              ((unsigned int)src[0] * sa * 255u + (unsigned int)dst[0] * da * inv_sa) / ao);
          dst[1] = (unsigned char)(
              ((unsigned int)src[1] * sa * 255u + (unsigned int)dst[1] * da * inv_sa) / ao);
          dst[2] = (unsigned char)(
              ((unsigned int)src[2] * sa * 255u + (unsigned int)dst[2] * da * inv_sa) / ao);
          dst[3] = (unsigned char)(ao / 255u);
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
            const uint32_t inv_sa = 65535u - (uint32_t)sa;
            const uint32_t da =
                (uint32_t)((uint16_t)dst[6] | ((uint16_t)dst[7] << 8));
            // See the eight-bit branch: the destination's alpha weights its
            // colour and the result is divided by the composite alpha.  This
            // used the opaque-destination simplification its own comment
            // described, which is wrong wherever the canvas is not yet fully
            // painted.
            const uint64_t ao =
                (uint64_t)sa * 65535u + (uint64_t)da * inv_sa; // sa > 0
            for (int c = 0; c < 4; c++) {
              uint16_t sc =
                  (uint16_t)src[c * 2] | ((uint16_t)src[c * 2 + 1] << 8);
              uint16_t dc =
                  (uint16_t)dst[c * 2] | ((uint16_t)dst[c * 2 + 1] << 8);
              uint32_t v;
              if (c < 3) {
                const uint64_t sum = (uint64_t)sc * (uint64_t)sa * 65535u +
                    (uint64_t)dc * (uint64_t)da * inv_sa;
                v = (uint32_t)(sum / ao);
              }
              else {
                v = (uint32_t)(ao / 65535u);
              }
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
      bool use_trns = (state->trns && state->trns_size > 0);
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
      // APNG canvas loop: for each frame up to the requested index, apply
      // previous frame's dispose (BACKGROUND or PREVIOUS), then save rect for
      // PREVIOUS if needed, decode this frame, and blend onto canvas.
      for (size_t i = 0; i <= item->index; i++) {
        const gimg_png_fctl_t * fctl = &state->frames[i].fctl;
        uint32_t fx = fctl->x_offset;
        uint32_t fy = fctl->y_offset;
        uint32_t fw = fctl->width;
        uint32_t fh = fctl->height;
        // Clipped for the same reason the blend is: these write the canvas at
        // the frame's own rectangle, so a rectangle past the edge is a write
        // past the end of the buffer. The loader refuses such a frame; this is
        // the second line of that defense and not a substitute for it.
        if (fx >= canvas_w || fy >= canvas_h) {
          fw = fh = 0;
        }
        else {
          if (fw > canvas_w - fx) {
            fw = canvas_w - fx;
          }
          if (fh > canvas_h - fy) {
            fh = canvas_h - fy;
          }
        }
        if (i > 0) {
          const gimg_png_fctl_t * prev_fctl = &state->frames[i - 1].fctl;
          uint32_t px = prev_fctl->x_offset;
          uint32_t py = prev_fctl->y_offset;
          uint32_t pw = prev_fctl->width;
          uint32_t ph = prev_fctl->height;
          if (px >= canvas_w || py >= canvas_h) {
            pw = ph = 0;
          }
          else {
            if (pw > canvas_w - px) {
              pw = canvas_w - px;
            }
            if (ph > canvas_h - py) {
              ph = canvas_h - py;
            }
          }
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
        gimg_png_apng_blend_frame(canvas, canvas_stride, canvas_w, canvas_h,
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
  bool use_trns = (state->trns && state->trns_size > 0);
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
