/**
 * @file
 *
 * JPEG save: baseline encode (DCT, quantisation, zigzag, Huffman); emit SOI,
 * DQT, DHT, SOF0, SOS, EOI. Supports grayscale and YCbCr 4:4:4. Synthetic
 * document support: when doc was not loaded by a codec, pixel data from
 * gimg_item_raster(item) only.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../meta/exif_internal.h"
#include "../../raster/raster_internal.h"
#include "../codec_internal.h"
#include "jpeg_internal.h"

/** Default quality when not specified (1..100). */
#define GIMG_JPEG_DEFAULT_QUALITY 85u

/** Write a 2-byte segment length (big-endian). */
static GIMG_Result jpeg_write_u16(
    GIMG_Stream * stream, uint16_t val, size_t * out_n) {
  unsigned char buf[2] = {
      (unsigned char)(val >> 8), (unsigned char)(val & 0xFF)};
  size_t n = 0;
  GIMG_Result r = gimg_stream_write(stream, buf, 2, &n);
  if (out_n) {
    *out_n += n;
  }
  return r;
}

/** Write marker (0xFF + byte). */
static GIMG_Result jpeg_write_marker(
    GIMG_Stream * stream, uint8_t marker, size_t * out_n) {
  unsigned char buf[2] = {0xFF, marker};
  size_t n = 0;
  GIMG_Result r = gimg_stream_write(stream, buf, 2, &n);
  if (out_n) {
    *out_n += n;
  }
  return r;
}

/** Write APP segment: marker + length (2 + payload_len) + payload. */
static GIMG_Result jpeg_write_app_segment(GIMG_Stream * stream, uint8_t marker,
    const void * payload, size_t payload_len, size_t * out_n) {
  if (payload_len > 65533u) {
    return GIMG_ERR_LIMIT;
  }
  GIMG_Result r = jpeg_write_marker(stream, marker, out_n);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_u16(stream, (uint16_t)(2 + payload_len), out_n);
  if (r != GIMG_OK) {
    return r;
  }
  if (payload_len > 0 && payload) {
    size_t n = 0;
    r = gimg_stream_write(stream, payload, payload_len, &n);
    if (out_n) {
      *out_n += n;
    }
    if (r != GIMG_OK) {
      return r;
    }
  }
  return GIMG_OK;
}

/** Build minimal APP0 JFIF (16 bytes). If x_dpi and y_dpi are both 0, use
 * units=0 (no units) and density 1,1; else units=1 (dots per inch). */
static void jpeg_build_minimal_app0(
    unsigned char * buf, uint32_t x_dpi, uint32_t y_dpi) {
  memcpy(buf, "JFIF\0", 5);
  buf[5] = 0x01;
  buf[6] = 0x01;
  if (x_dpi == 0 && y_dpi == 0) {
    buf[7] = 0; // no units
    buf[8] = 0;
    buf[9] = 1;
    buf[10] = 0;
    buf[11] = 1;
  }
  else {
    if (x_dpi == 0) {
      x_dpi = 1;
    }
    if (y_dpi == 0) {
      y_dpi = 1;
    }
    buf[7] = 1; // dots per inch
    buf[8] = (unsigned char)(x_dpi >> 8);
    buf[9] = (unsigned char)(x_dpi & 0xFFu);
    buf[10] = (unsigned char)(y_dpi >> 8);
    buf[11] = (unsigned char)(y_dpi & 0xFFu);
  }
  buf[12] = 0;
  buf[13] = 0; // no thumbnail
  buf[14] = 0;
  buf[15] = 0;
}

/** RGB to YCbCr (BT.601). R,G,B 0..255 -> Y,Cb,Cr 0..255. */
static void jpeg_rgb_to_ycbcr(
    uint8_t r, uint8_t g, uint8_t b, uint8_t * y, uint8_t * cb, uint8_t * cr) {
  int ri = (int)r;
  int gi = (int)g;
  int bi = (int)b;
  int yv = (77 * ri + 150 * gi + 29 * bi + 128) / 256;
  int cbv = (-43 * ri - 84 * gi + 127 * bi + 128 * 256) / 256 + 128;
  int crv = (127 * ri - 106 * gi - 21 * bi + 128 * 256) / 256 + 128;
  *y = (uint8_t)(yv < 0 ? 0 : (yv > 255 ? 255 : yv));
  *cb = (uint8_t)(cbv < 0 ? 0 : (cbv > 255 ? 255 : cbv));
  *cr = (uint8_t)(crv < 0 ? 0 : (crv > 255 ? 255 : crv));
}

GIMG_Result gimg_jpeg_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  report->bytes_written = 0;
  if (gimg_doc_item_count(doc) == 0) {
    return GIMG_ERR_UNSUPPORTED; // No image to save.
  }
  GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, 0);
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }

  // Synthetic document support: use raster if present; else decode only when
  // doc was loaded by this codec.
  GIMG_Raster * raster = gimg_item_raster(item);
  int raster_owned = 0;
  if (!raster && doc->loaded_by_codec == (struct GIMG_Codec *)codec) {
    GIMG_Result r = gimg_item_decode(item, NULL, &raster);
    if (r != GIMG_OK || !raster) {
      return (r != GIMG_OK) ? r : GIMG_ERR_UNSUPPORTED;
    }
    raster_owned = 1;
  }
  if (!raster) {
    return GIMG_ERR_UNSUPPORTED; // No raster and not loaded by us.
  }

  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || width == 0 || height == 0) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  if (width > GIMG_JPEG_MAX_DIMENSION || height > GIMG_JPEG_MAX_DIMENSION) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_LIMIT;
  }

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count(width, height, &pixel_count) != GIMG_OK) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_LIMIT;
  }

  int num_components = 0;
  if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count == 1 &&
      fmt->bits_per_channel[0] == 8) {
    num_components = 1;
  }
  else if ((fmt->channel_model == GIMG_CHANNEL_RGB ||
               fmt->channel_model == GIMG_CHANNEL_RGBA) &&
      fmt->channel_count >= 3 && fmt->bits_per_channel[0] == 8 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    num_components = 3;
  }
  else {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }

  const GIMG_Allocator * alloc = codec->allocator;
  alloc = gimg_alloc_or_default(alloc);
  size_t stride_bytes = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  if (!pixels) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }

  // Allocate component buffers (Y only or Y, Cb, Cr). 4:4:4 so same size.
  size_t comp_size = 0;
  if (!gimg_safe_mul_size((size_t)width, (size_t)height, &comp_size)) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_LIMIT;
  }
  unsigned char * comp_y = (unsigned char *)gimg_malloc(alloc, comp_size);
  if (!comp_y) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_OOM;
  }
  unsigned char * comp_cb = NULL;
  unsigned char * comp_cr = NULL;
  if (num_components == 3) {
    comp_cb = (unsigned char *)gimg_malloc(alloc, comp_size);
    comp_cr = (unsigned char *)gimg_malloc(alloc, comp_size);
    if (!comp_cb || !comp_cr) {
      gimg_free(alloc, comp_y);
      if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      return GIMG_ERR_OOM;
    }
  }

  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  if (num_components == 1) {
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        comp_y[y * (size_t)width + x] = row[x * bpp];
      }
    }
  }
  else {
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        uint8_t r = row[x * bpp + 0];
        uint8_t g = row[x * bpp + 1];
        uint8_t b = row[x * bpp + 2];
        uint8_t yv, cb, cr;
        jpeg_rgb_to_ycbcr(r, g, b, &yv, &cb, &cr);
        comp_y[y * (size_t)width + x] = yv;
        comp_cb[y * (size_t)width + x] = cb;
        comp_cr[y * (size_t)width + x] = cr;
      }
    }
  }

  unsigned quality = GIMG_JPEG_DEFAULT_QUALITY;
  if (options && options->quality != 0) {
    quality = options->quality;
    if (quality > 100) {
      quality = 100;
    }
  }

  uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES];
  uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES];
  gimg_jpeg_default_quant_scaled(quality, quant_luma, quant_chroma);

  unsigned char * scan_data = NULL;
  size_t scan_size = 0;
  GIMG_Result r = gimg_jpeg_encode_baseline_scan(width, height, num_components,
      comp_y, comp_cb, comp_cr, (size_t)width, (size_t)width, (size_t)width,
      quant_luma, quant_chroma, alloc, &scan_data, &scan_size);

  gimg_free(alloc, comp_y);
  gimg_free(alloc, comp_cb);
  gimg_free(alloc, comp_cr);
  if (raster_owned) {
    gimg_raster_destroy(raster);
  }
  if (r != GIMG_OK) {
    return r;
  }
  if (!scan_data) {
    return GIMG_ERR_OOM;
  }

  size_t written = 0;
  r = gimg_stream_write(
      stream, gimg_jpeg_signature, GIMG_JPEG_SIGNATURE_LEN, &written);
  if (r != GIMG_OK) {
    gimg_free(alloc, scan_data);
    return r;
  }
  report->bytes_written += written;

  // APP segments per metadata policy (order: COM, APP0, APP1 EXIF, APP1 XMP,
  // APP2 ICC). COM and APP segments before SOF.
  {
    GIMG_Meta_Policy policy =
        options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
    uint32_t x_dpi = 0, y_dpi = 0;
    GIMG_Meta_Common * meta_common = gimg_doc_meta_common(doc);
    if (meta_common) {
      gimg_meta_common_dpi(meta_common, &x_dpi, &y_dpi);
    }
    GIMG_Meta_Raw * meta_raw = gimg_doc_meta_raw(doc);

    // COM segment(s): when policy preserves metadata, write in same order as
    // load (stored as concatenated 2-byte BE length + payload per COM).
    if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_COMMON_ONLY &&
        meta_raw) {
      size_t com_size = 0;
      if (gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_COM, NULL,
              &com_size) == GIMG_OK &&
          com_size > 0) {
        unsigned char * com_buf =
            (unsigned char *)gimg_malloc(alloc, com_size);
        if (com_buf) {
          r = gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_COM, com_buf,
              &com_size);
          if (r == GIMG_OK) {
            size_t offset = 0;
            while (offset + 2 <= com_size) {
              uint16_t plen =
                  (uint16_t)((com_buf[offset] << 8) | com_buf[offset + 1]);
              offset += 2;
              if (offset + plen > com_size) {
                break;
              }
              r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_COM,
                  com_buf + offset, plen, &report->bytes_written);
              if (r != GIMG_OK) {
                gimg_free(alloc, com_buf);
                gimg_free(alloc, scan_data);
                return r;
              }
              offset += plen;
            }
          }
          gimg_free(alloc, com_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, scan_data);
          return r;
        }
      }
    }

    // APP0: from meta_raw when preserving and present, else minimal (JFIF).
    size_t app0_len = 0;
    int have_app0 = (policy != GIMG_META_DROP_ALL &&
                        policy != GIMG_META_KEEP_COMMON_ONLY) &&
        meta_raw &&
        gimg_meta_raw_get(
            meta_raw, "jpeg", GIMG_JPEG_RAW_APP0, NULL, &app0_len) == GIMG_OK &&
        app0_len > 0;
    if (have_app0) {
      unsigned char * app0_buf = (unsigned char *)gimg_malloc(alloc, app0_len);
      if (app0_buf) {
        r = gimg_meta_raw_get(
            meta_raw, "jpeg", GIMG_JPEG_RAW_APP0, app0_buf, &app0_len);
        if (r == GIMG_OK) {
          r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP0, app0_buf,
              app0_len, &report->bytes_written);
        }
        gimg_free(alloc, app0_buf);
      }
      if (r != GIMG_OK) {
        gimg_free(alloc, scan_data);
        return r;
      }
    }
    else {
      unsigned char app0[16];
      jpeg_build_minimal_app0(app0, x_dpi, y_dpi);
      r = jpeg_write_app_segment(
          stream, GIMG_JPEG_MARKER_APP0, app0, 16, &report->bytes_written);
      if (r != GIMG_OK) {
        gimg_free(alloc, scan_data);
        return r;
      }
    }

    // APP1 EXIF / XMP and APP2 ICC only when policy preserves metadata
    if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_COMMON_ONLY) {
      // APP1 EXIF (optionally STRIP_GPS or NORMALIZE_EXIF)
      {
        size_t exif_size = 0;
        if (meta_raw &&
            gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF, NULL,
                &exif_size) == GIMG_OK &&
            exif_size > 0) {
          unsigned char * exif_buf =
              (unsigned char *)gimg_malloc(alloc, exif_size);
          if (exif_buf) {
            r = gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF,
                exif_buf, &exif_size);
            if (r == GIMG_OK) {
              const void * to_write = exif_buf;
              size_t to_write_size = exif_size;
              void * modified = NULL;
              size_t modified_size = 0;
              if (policy == GIMG_META_STRIP_GPS) {
                if (gimg_exif_strip_gps(alloc, exif_buf, exif_size, &modified,
                        &modified_size) == GIMG_OK) {
                  to_write = modified;
                  to_write_size = modified_size;
                }
              }
              else if (policy == GIMG_META_NORMALIZE_EXIF) {
                if (gimg_exif_normalize(alloc, exif_buf, exif_size, &modified,
                        &modified_size) == GIMG_OK) {
                  to_write = modified;
                  to_write_size = modified_size;
                }
              }
              r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP1,
                  to_write, to_write_size, &report->bytes_written);
              if (modified) {
                gimg_free(alloc, modified);
              }
            }
            gimg_free(alloc, exif_buf);
          }
          if (r != GIMG_OK) {
            gimg_free(alloc, scan_data);
            return r;
          }
        }
      }

      // APP1 XMP
      size_t xmp_size = 0;
      if (meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_XMP, NULL,
              &xmp_size) == GIMG_OK &&
          xmp_size > 0) {
        unsigned char * xmp_buf = (unsigned char *)gimg_malloc(alloc, xmp_size);
        if (xmp_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_XMP, xmp_buf, &xmp_size);
          if (r == GIMG_OK) {
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP1, xmp_buf,
                xmp_size, &report->bytes_written);
          }
          gimg_free(alloc, xmp_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, scan_data);
          return r;
        }
      }

      // APP2 ICC
      size_t icc_size = 0;
      if (meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC, NULL,
              &icc_size) == GIMG_OK &&
          icc_size > 0) {
        unsigned char * icc_buf = (unsigned char *)gimg_malloc(alloc, icc_size);
        if (icc_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC, icc_buf, &icc_size);
          if (r == GIMG_OK) {
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP2, icc_buf,
                icc_size, &report->bytes_written);
          }
          gimg_free(alloc, icc_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, scan_data);
          return r;
        }
      }
    }
  }

  // DQT: table 0 (luma), table 1 (chroma). Lq = 2 + 1 + 64 = 67 for 8-bit.
  {
    unsigned char dqt0[67];
    memset(dqt0, 0, sizeof(dqt0));
    dqt0[0] = 0x00; // Pq=0 (8-bit), Tq=0
    for (int i = 0; i < 64; i++) {
      dqt0[1 + i] = (unsigned char)(quant_luma[i] > 255 ? 255 : quant_luma[i]);
    }
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DQT, &report->bytes_written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    r = jpeg_write_u16(stream, 67, &report->bytes_written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    r = gimg_stream_write(stream, dqt0, sizeof(dqt0), &written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    report->bytes_written += written;

    if (num_components == 3) {
      unsigned char dqt1[67];
      memset(dqt1, 0, sizeof(dqt1));
      dqt1[0] = 0x01; // Tq=1
      for (int i = 0; i < 64; i++) {
        dqt1[1 + i] =
            (unsigned char)(quant_chroma[i] > 255 ? 255 : quant_chroma[i]);
      }
      r = jpeg_write_marker(
          stream, GIMG_JPEG_MARKER_DQT, &report->bytes_written);
      if (r != GIMG_OK) {
        gimg_free(alloc, scan_data);
        return r;
      }
      r = jpeg_write_u16(stream, 67, &report->bytes_written);
      if (r != GIMG_OK) {
        gimg_free(alloc, scan_data);
        return r;
      }
      r = gimg_stream_write(stream, dqt1, sizeof(dqt1), &written);
      if (r != GIMG_OK) {
        gimg_free(alloc, scan_data);
        return r;
      }
      report->bytes_written += written;
    }
  }

  // DHT: standard tables
  {
    size_t dht_written = 0;
    r = gimg_jpeg_write_standard_dht(stream, &dht_written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    report->bytes_written += dht_written;
  }

  // SOF0
  {
    uint16_t sof_len = (uint16_t)(8 + 3 * (uint16_t)num_components);
    r = jpeg_write_marker(
        stream, GIMG_JPEG_MARKER_SOF0, &report->bytes_written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    r = jpeg_write_u16(stream, sof_len, &report->bytes_written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    unsigned char sof[8 + 3 * 4];
    memset(sof, 0, sizeof(sof));
    sof[0] = 8; // precision
    sof[1] = (unsigned char)(height >> 8);
    sof[2] = (unsigned char)(height & 0xFF);
    sof[3] = (unsigned char)(width >> 8);
    sof[4] = (unsigned char)(width & 0xFF);
    sof[5] = (unsigned char)num_components;
    if (num_components == 1) {
      sof[6] = 0x01; // C1=1
      sof[7] = 0x11; // H=1, V=1
      sof[8] = 0x00; // Tq=0
    }
    else {
      sof[6] = 0x01;
      sof[7] = 0x11;
      sof[8] = 0x00;
      sof[9] = 0x02;
      sof[10] = 0x11;
      sof[11] = 0x01;
      sof[12] = 0x03;
      sof[13] = 0x11;
      sof[14] = 0x01;
    }
    r = gimg_stream_write(
        stream, sof, 8 + 3 * (size_t)num_components, &written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    report->bytes_written += written;
  }

  // SOS per T.81 Annex B: Ls = 2 + (1 + 2*Ns + 3) = 6 + 2*Ns; payload ends with
  // Ss (1), Se (1), and one byte Ah (high 4 bits) | Al (low 4 bits).
  {
    uint16_t sos_len = (uint16_t)(6 + 2 * (uint16_t)num_components);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_SOS, &report->bytes_written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    r = jpeg_write_u16(stream, sos_len, &report->bytes_written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    unsigned char sos[12];
    memset(sos, 0, sizeof(sos));
    sos[0] = (unsigned char)num_components;
    if (num_components == 1) {
      sos[1] = 0x01;  // Cs=1
      sos[2] = 0x00;  // Td=0, Ta=0
    }
    else {
      sos[1] = 0x01;
      sos[2] = 0x00;  // Td=0, Ta=0
      sos[3] = 0x02;
      sos[4] = 0x11;  // Td=1, Ta=1
      sos[5] = 0x03;
      sos[6] = 0x11;
    }
    size_t tail = 1 + 2 * (size_t)num_components;
    sos[tail] = 0x00;      // Ss
    sos[tail + 1] = 0x3F;  // Se
    sos[tail + 2] = 0x00;  // Ah (high nibble) | Al (low nibble) = 0 for baseline
    r = gimg_stream_write(stream, sos, tail + 3, &written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    report->bytes_written += written;
  }

  // Scan data (already byte-stuffed in encoder)
  r = gimg_stream_write(stream, scan_data, scan_size, &written);
  gimg_free(alloc, scan_data);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written += written;

  r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_EOI, &report->bytes_written);
  if (r != GIMG_OK) {
    return r;
  }
  return GIMG_OK;
}
