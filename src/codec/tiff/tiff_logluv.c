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
 * SGI LogLuv: compressions 34676 and 34677, photometrics 32844 and 32845.
 *
 * Greg Larson's encodings of absolute luminance, which TIFF carries in three
 * forms.  The sample encodings themselves - what a code means - are in
 * libs/color (gcol_logl16_to_y, gcol_logluv32_to_xyz, gcol_logluv24_to_xyz);
 * this file is only how the codes are stored:
 *
 *   34676 with LogL    16-bit codes; with LogLuv, 32-bit codes.  Each row is
 *                      coded byte plane by byte plane, most significant
 *                      first, each plane a run-length stream: a control byte
 *                      of 128 or more is a run of (control - 126) copies of
 *                      the byte after it, and one below 128 is that many
 *                      literal bytes (zero is a no-op).
 *   34677 with LogLuv  24-bit codes, three bytes a pixel, most significant
 *                      first, no coding at all.
 *
 * Neither depends on the file's byte order: the planes are byte streams, and
 * the 24-bit packing is fixed.  The file's BitsPerSample and SampleFormat
 * are not read, because they describe the buffer the writing program handed
 * libtiff - 32-bit float, or 16-bit signed - and not anything stored.
 *
 * What a decoded sample becomes is a float raster (GIMG_PIXEL_GRAYF32 or
 * GIMG_PIXEL_RGBAF32): Y in the file's units, or XYZ taken to linear RGB in
 * sRGB's primaries.  Nothing is tone mapped or clamped, because LogLuv
 * exists to hold what an integer raster cannot.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/color/math.h>

#include "tiff_internal.h"

bool gimg_tiff_is_logluv(const gimg_tiff_ifd_t * ifd) {
  return ifd->photometric == GIMG_TIFF_PHOTOMETRIC_LOGL ||
      ifd->photometric == GIMG_TIFF_PHOTOMETRIC_LOGLUV ||
      ifd->compression == GIMG_TIFF_COMPRESSION_SGILOG ||
      ifd->compression == GIMG_TIFF_COMPRESSION_SGILOG24;
}

const char * gimg_tiff_logluv_refusal(
    const gimg_tiff_ifd_t * ifd, GIMG_Result * out_code) {
  *out_code = GIMG_ERR_CORRUPT;
  const bool logl = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_LOGL;
  const bool logluv = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_LOGLUV;
  if (!logl && !logluv) {
    return "SGILog compression on samples that are not LogL or LogLuv, "
           "which it has no way to code";
  }
  if (ifd->compression != GIMG_TIFF_COMPRESSION_SGILOG &&
      ifd->compression != GIMG_TIFF_COMPRESSION_SGILOG24) {
    return "LogL or LogLuv samples outside SGILog compression, which is the "
           "only place they are defined";
  }
  if (logl && ifd->compression == GIMG_TIFF_COMPRESSION_SGILOG24) {
    return "LogL in 24-bit SGILog, which codes only LogLuv";
  }
  if (ifd->samples_per_pixel != (logl ? 1u : 3u)) {
    return logl ? "LogL with other than one sample"
                : "LogLuv with other than three samples";
  }
  *out_code = GIMG_ERR_UNSUPPORTED;
  if (ifd->planar_config != 1u) {
    // A LogLuv code is one pixel's luminance and chromaticity together;
    // there are no separate planes to store, and no writer stores them.
    return "LogLuv with PlanarConfiguration 2; not read";
  }
  if (ifd->predictor != 1u) {
    return "a Predictor on LogLuv codes, which differencing does not apply to";
  }
  *out_code = GIMG_OK;
  return NULL;
}

/** Bytes in one stored code: 2 for LogL, 4 for LogLuv32, 3 for LogLuv24. */
static size_t logluv_code_bytes(const gimg_tiff_ifd_t * ifd) {
  if (ifd->compression == GIMG_TIFF_COMPRESSION_SGILOG24) {
    return 3u;
  }
  return ifd->photometric == GIMG_TIFF_PHOTOMETRIC_LOGL ? 2u : 4u;
}

size_t gimg_tiff_logluv_rows(const gimg_tiff_ifd_t * ifd,
    const unsigned char * stored, size_t stored_size, size_t width,
    size_t rows, uint32_t * codes) {
  const size_t nbytes = logluv_code_bytes(ifd);
  size_t at = 0;

  if (ifd->compression == GIMG_TIFF_COMPRESSION_SGILOG24) {
    for (size_t row = 0; row < rows; row++) {
      if (stored_size - at < width * 3u) {
        return row;
      }
      uint32_t * out = codes + (row * width);
      for (size_t i = 0; i < width; i++, at += 3u) {
        out[i] = ((uint32_t)stored[at] << 16) |
            ((uint32_t)stored[at + 1u] << 8) | (uint32_t)stored[at + 2u];
      }
    }
    return rows;
  }

  for (size_t row = 0; row < rows; row++) {
    uint32_t * out = codes + (row * width);
    for (size_t i = 0; i < width; i++) {
      out[i] = 0u;
    }
    for (size_t plane = 0; plane < nbytes; plane++) {
      const unsigned shift = (unsigned)((nbytes - 1u - plane) * 8u);
      size_t i = 0;
      while (i < width) {
        if (at >= stored_size) {
          return row; // The block ended inside this row.
        }
        const unsigned control = stored[at++];
        if (control >= 128u) {
          if (at >= stored_size) {
            return row;
          }
          const uint32_t b = (uint32_t)stored[at++] << shift;
          // A run past the row's end is clipped to it: a row ends when it is
          // full, and what follows begins the next plane.
          for (size_t n = control - 126u; n > 0u && i < width; n--) {
            out[i++] |= b;
          }
        }
        else {
          for (size_t n = control; n > 0u && i < width; n--) {
            if (at >= stored_size) {
              return row;
            }
            out[i++] |= (uint32_t)stored[at++] << shift;
          }
        }
      }
    }
  }
  return rows;
}

void gimg_tiff_logluv_to_float(const gimg_tiff_ifd_t * ifd,
    const uint32_t * codes, size_t pixels, const double xyz_to_rgb[9],
    float * out) {
  if (ifd->photometric == GIMG_TIFF_PHOTOMETRIC_LOGL) {
    for (size_t i = 0; i < pixels; i++) {
      out[i] = (float)gcol_logl16_to_y((uint16_t)codes[i]);
    }
    return;
  }
  const bool narrow = ifd->compression == GIMG_TIFF_COMPRESSION_SGILOG24;
  const double * m = xyz_to_rgb;
  for (size_t i = 0; i < pixels; i++) {
    double xyz[3];
    if (narrow) {
      (void)gcol_logluv24_to_xyz(codes[i], xyz);
    }
    else {
      (void)gcol_logluv32_to_xyz(codes[i], xyz);
    }
    float * p = out + (i * 4u);
    p[0] = (float)(m[0] * xyz[0] + m[1] * xyz[1] + m[2] * xyz[2]);
    p[1] = (float)(m[3] * xyz[0] + m[4] * xyz[1] + m[5] * xyz[2]);
    p[2] = (float)(m[6] * xyz[0] + m[7] * xyz[1] + m[8] * xyz[2]);
    p[3] = 1.0f;
  }
}
