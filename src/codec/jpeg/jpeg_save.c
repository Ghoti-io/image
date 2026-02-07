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
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
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
  // Save_Options has no quality field yet; could use reserved or extend later.
  (void)options;

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

  // Optional minimal APP0 JFIF
  {
    unsigned char app0[] = {0xFF, GIMG_JPEG_MARKER_APP0, 0x00, 0x10, 'J', 'F',
        'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    r = gimg_stream_write(stream, app0, sizeof(app0), &written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    report->bytes_written += written;
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

  // SOS: Ls = 2 + 1 + 2*Ns + 4 = 7 + 2*Ns
  {
    uint16_t sos_len = (uint16_t)(7 + 2 * (uint16_t)num_components);
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
    unsigned char sos[13];
    memset(sos, 0, sizeof(sos));
    sos[0] = (unsigned char)num_components;
    if (num_components == 1) {
      sos[1] = 0x01; // Cs=1
      sos[2] = 0x00; // Td=0, Ta=0
    }
    else {
      sos[1] = 0x01;
      sos[2] = 0x00; // Td=0, Ta=0
      sos[3] = 0x02;
      sos[4] = 0x11; // Td=1, Ta=1
      sos[5] = 0x03;
      sos[6] = 0x11;
    }
    sos[1 + 2 * (size_t)num_components] = 0x00; // Ss
    sos[2 + 2 * (size_t)num_components] = 0x3F; // Se
    sos[3 + 2 * (size_t)num_components] = 0x00; // Ah
    sos[4 + 2 * (size_t)num_components] = 0x00; // Al
    r = gimg_stream_write(
        stream, sos, 1 + 2 * (size_t)num_components + 4, &written);
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
