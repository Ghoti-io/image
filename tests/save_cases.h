/**
 * @file
 *
 * The writer configurations worth sweeping, and a raster to feed them.
 *
 * Two sweeps walk this same list - one injecting an allocation failure, one a
 * sink that runs out of room - and they ask different questions of the same
 * code.  Keeping the list in one place is what makes both of them widen the
 * day somebody adds a writer option: a case added here is a case both sweeps
 * start running, rather than one sweep learning about it and the other not.
 *
 * The cases are not one writer with a size swept over it.  Each reaches a
 * different half of the JPEG writer - a progressive scan script, a lossless
 * frame, a hierarchical pyramid, a twelve-bit frame, a CMYK frame with its
 * Adobe marker, a non-interleaved scan order - and a sweep is only ever as
 * wide as the paths its fixtures walk.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_TESTS_SAVE_CASES_H
#define GHOTI_IO_GIMG_TESTS_SAVE_CASES_H

#include <cstdint>
#include <cstring>
#include <vector>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>

#include "exif_test_utils.h"
#include "../src/codec/jpeg/jpeg_internal.h"
#include "../src/codec/png/png_internal.h"

namespace gimg_test {

/**
 * Metadata attached to a document after it is built, or nullptr.
 *
 * Every APP-segment writer in the JPEG save path, and every ancillary-chunk
 * writer in the PNG one, runs only when the document carries the thing it
 * writes.  A document made from a bare raster reaches none of them, so the
 * sweeps saw the frame and nothing around it.  These hang the metadata on so
 * the segment writers - and the arms that free their buffers when a write
 * fails - are on the path.
 */
using Decorate = void (*)(GIMG_Doc *);

/** Two COM segments, stored the way the loader stores them: (2B BE len + payload)*. */
inline std::vector<uint8_t> two_comments(void) {
  const char * texts[2] = {"first comment", "and a second one"};
  std::vector<uint8_t> out;
  for (const char * t : texts) {
    const size_t n = std::strlen(t);
    out.push_back((uint8_t)(n >> 8));
    out.push_back((uint8_t)(n & 0xFFu));
    out.insert(out.end(), t, t + n);
  }
  return out;
}

/** An ICC profile split over two APP2 segments: 2B count, then (2B len + payload)*. */
inline std::vector<uint8_t> icc_in_two_chunks(void) {
  std::vector<uint8_t> out = {0x00, 0x02};
  for (int i = 0; i < 2; i++) {
    const std::vector<uint8_t> part(40u, (uint8_t)(0xA0 + i));
    out.push_back((uint8_t)(part.size() >> 8));
    out.push_back((uint8_t)(part.size() & 0xFFu));
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

/** Two APPn segments the reader did not recognise: (1B marker + 2B BE len + payload)*. */
inline std::vector<uint8_t> two_unknown_apps(void) {
  std::vector<uint8_t> out;
  for (uint8_t marker : {(uint8_t)0xE5u, (uint8_t)0xE7u}) {
    const std::vector<uint8_t> payload(12u, marker);
    out.push_back(marker);
    out.push_back((uint8_t)(payload.size() >> 8));
    out.push_back((uint8_t)(payload.size() & 0xFFu));
    out.insert(out.end(), payload.begin(), payload.end());
  }
  return out;
}

/** Every APP segment the JPEG writer can be asked to put back. */
inline void attach_jpeg_app_segments(GIMG_Doc * doc) {
  GIMG_Meta_Raw * raw = nullptr;
  if (gimg_doc_ensure_meta_raw(doc, &raw) != GIMG_OK || !raw) { return; }
  const std::vector<uint8_t> com = two_comments();
  gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_COM, com.data(), com.size());
  // JFXX extension code 0x10: a JPEG-compressed thumbnail follows.  The
  // writer copies the payload through without reading it.
  const std::vector<uint8_t> jfxx = {'J', 'F', 'X', 'X', 0, 0x10, 0xFF, 0xD8,
      0xFF, 0xD9};
  gimg_meta_raw_attach(
      raw, "jpeg", GIMG_JPEG_RAW_APP0_JFXX, jfxx.data(), jfxx.size());
  const char xmp[] = "http://ns.adobe.com/xap/1.0/\0<x:xmpmeta/>";
  gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP1_XMP, xmp, sizeof(xmp));
  const std::vector<uint8_t> chunks = icc_in_two_chunks();
  gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC_CHUNKS,
      chunks.data(), chunks.size());
  const std::vector<uint8_t> app13(24u, 0x31u);
  gimg_meta_raw_attach(
      raw, "jpeg", GIMG_JPEG_RAW_APP13, app13.data(), app13.size());
  const std::vector<uint8_t> app14 = {'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0,
      0, 0};
  gimg_meta_raw_attach(
      raw, "jpeg", GIMG_JPEG_RAW_APP14, app14.data(), app14.size());
  const std::vector<uint8_t> unknown = two_unknown_apps();
  gimg_meta_raw_attach(
      raw, "jpeg", GIMG_JPEG_RAW_APP_UNKNOWN, unknown.data(), unknown.size());
}

/** A real Exif blob, with the APP1 prefix the writer expects to find on it. */
inline void attach_exif(GIMG_Doc * doc) {
  GIMG_Meta_Raw * raw = nullptr;
  if (gimg_doc_ensure_meta_raw(doc, &raw) != GIMG_OK || !raw) { return; }
  const std::vector<uint8_t> exif = exif_test::make_exif_with_gps();
  std::vector<uint8_t> app1 = {'E', 'x', 'i', 'f', 0, 0};
  app1.insert(app1.end(), exif.begin(), exif.end());
  gimg_meta_raw_attach(
      raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF, app1.data(), app1.size());
  gimg_meta_raw_attach(
      raw, "png", (uint32_t)GIMG_PNG_eXIf, exif.data(), exif.size());
}

/**
 * A colour the writer has to state but was not handed a profile for.
 *
 * The JPEG writer synthesizes an ICC profile from this and writes it as APP2,
 * which is a different path from copying one the file carried, and the only
 * one a document built in memory can take.
 */
inline void tag_adobe_rgb(GIMG_Doc * doc) {
  GIMG_Raster * raster = gimg_item_raster(gimg_doc_item(doc, 0));
  if (!raster) { return; }
  GIMG_Color_Info info;
  gimg_color_info_default(&info);
  info.primaries = GIMG_PRIMARIES_ADOBE_RGB;
  info.white_point = GIMG_PRIMARIES_ADOBE_RGB;
  info.transfer = GIMG_TRANSFER_GAMMA;
  info.gamma_value = 2.19921875;
  info.intent = GIMG_INTENT_RELATIVE_COLORIMETRIC;
  gimg_raster_set_color_info(raster, &info);
}

/** Description and resolution, which each format states in its own way. */
inline void attach_common(GIMG_Doc * doc) {
  GIMG_Meta_Common * meta = nullptr;
  if (gimg_doc_ensure_meta_common(doc, &meta) != GIMG_OK || !meta) { return; }
  gimg_meta_common_set_description(meta, "a description worth writing");
  gimg_meta_common_set_dpi(meta, 300u, 300u);
}

/**
 * A raster with something in it worth coding.
 *
 * A flat fill would quantize to a DC term and nothing else, and several of
 * the writer's arms only run when a block has AC coefficients to emit, so the
 * pattern is a gradient with a per-pixel perturbation: deterministic, but not
 * constant along either axis or within a block.
 */
inline GIMG_Raster * make_raster(const GIMG_Pixel_Format & fmt, uint32_t w,
    uint32_t h, unsigned levels, unsigned run = 1u) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, &fmt, GIMG_RASTER_OWNED, nullptr, 0, &r)
      != GIMG_OK) {
    return nullptr;
  }
  unsigned char * px = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  const unsigned ch = fmt.channel_count;
  const unsigned bits = fmt.bits_per_channel[0];
  const unsigned max = (1u << (bits > 12u ? 12u : bits)) - 1u;
  // GIF refuses an image with more colours than a palette holds, and refuses
  // a partly transparent pixel outright, so a case can ask for a coarser
  // pattern rather than for a generator of its own.  Asking for one also
  // makes the alpha channel opaque, since a varying alpha is the other half
  // of what GIF will not take.
  const unsigned span = (levels && levels <= max + 1u) ? levels : max + 1u;
  const bool opaque_alpha = levels != 0u && fmt.channel_model == GIMG_CHANNEL_RGBA;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      for (unsigned c = 0; c < ch; c++) {
        // `run` repeats each value along the row. Every run-length encoder
        // in these writers must beat writing the pixels plainly before it is
        // used, so a scattered pattern reaches none of them: a case that asks
        // for RLE and gets BI_RGB sweeps the plain writer under another name.
        // BmpEncode.EveryCompressionOptionReachesTheEncoderItNames is the
        // check that each option still selects what it says.
        const uint32_t bx = (run > 1u) ? (x / run) : x;
        const unsigned v = (opaque_alpha && c == 3u)
            ? max
            : ((bx * 7u + y * 13u + c * 29u) * 37u) % span;
        if (bits <= 8u) {
          px[y * stride + (x * ch + c)] = (unsigned char)v;
        }
        else {
          uint16_t * row = (uint16_t *)(px + y * stride);
          row[x * ch + c] = (uint16_t)v;
        }
      }
    }
  }
  return r;
}

/** One save configuration, named so a failure says which. */
struct SaveCase {
  const char * name;
  const char * codec;
  const GIMG_Pixel_Format * format;
  GIMG_Save_Options options;
  unsigned levels; ///< Distinct values per channel; 0 = the format's full range.
  Decorate decorate; ///< Metadata to hang on the document, or nullptr.
  unsigned run;      ///< Repeat each value this many times along a row; 0 = 1.
  /**
   * Smallest image this case needs, or 0 for the sweep's own size.
   *
   * RLE24 is the reason this exists. It is only eligible for a plan of more
   * than 256 colours, and only chosen when runs make it smaller than writing
   * the pixels plainly - so it needs more *blocks* than 256, which at a run
   * of four is more pixels than either sweep's default. A case that cannot
   * say so is a case that silently sweeps the plain writer instead.
   */
  uint32_t min_side;
};

/**
 * libjpeg's jpeg_simple_progression: the DC sent a bit short and refined, and
 * each AC band sent two bits short and refined twice.  The encoder's own
 * default holds no bits back at all, so without this the successive
 * approximation writers - four of them, plus an arithmetic set - are not on
 * the path the sweeps walk.
 */
inline const GIMG_JPEG_Progressive_Config * simple_progression(void) {
  static const GIMG_JPEG_Progressive_Scan scans[] = {
      {0, 0, 0, 1},
      {1, 5, 0, 2},
      {6, 63, 0, 2},
      {1, 63, 2, 1},
      {0, 0, 1, 0},
      {1, 63, 1, 0},
  };
  static const GIMG_JPEG_Progressive_Config config = {6u, scans};
  return &config;
}

/** Every writer configuration the sweeps walk. */
inline std::vector<SaveCase> save_cases(void) {
  auto opt = [](void) {
    GIMG_Save_Options o = {};
    o.quality = 80;
    return o;
  };

  std::vector<SaveCase> cases;
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg baseline gray", "jpeg", &GIMG_PIXEL_GRAY8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg baseline rgb 4:2:0", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444;
    cases.push_back({"jpeg rgb 4:4:4", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_progressive = 1;
    cases.push_back({"jpeg progressive rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_arithmetic = 1;
    cases.push_back({"jpeg arithmetic rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_restart_interval = 2;
    cases.push_back({"jpeg restarts rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_non_interleaved = 1;
    cases.push_back({"jpeg non-interleaved rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_lossless_predictor = 1;
    cases.push_back({"jpeg lossless rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_hierarchical_levels = 1;
    cases.push_back({"jpeg hierarchical gray", "jpeg", &GIMG_PIXEL_GRAY8, o, 0u, nullptr, 0u, 0u});
  }
  // Each option above is set on its own, and the writer's header code is not
  // organised that way: a lossless or hierarchical frame writes its own DAC,
  // DRI and Adobe segments through separate code from the baseline frame's.
  // Setting one option at a time left every one of those arms unwritten, so
  // both sweeps walked past them.  These are the combinations, not more of
  // the same.
  {
    GIMG_Save_Options o = opt();
    o.jpeg_hierarchical_levels = 1;
    o.jpeg_arithmetic = 1;
    cases.push_back({"jpeg hierarchical arithmetic rgb", "jpeg",
        &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_hierarchical_levels = 1;
    o.jpeg_restart_interval = 2;
    cases.push_back({"jpeg hierarchical restarts rgb", "jpeg",
        &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_lossless_predictor = 1;
    o.jpeg_arithmetic = 1;
    cases.push_back({"jpeg lossless arithmetic rgb", "jpeg", &GIMG_PIXEL_RGBA8,
        o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_lossless_predictor = 1;
    o.jpeg_restart_interval = 2;
    cases.push_back({"jpeg lossless restarts rgb", "jpeg", &GIMG_PIXEL_RGBA8, o,
        0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_precision = 12;
    cases.push_back({"jpeg 12-bit gray", "jpeg", &GIMG_PIXEL_GRAY12, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_precision = 12;
    cases.push_back({"jpeg 12-bit rgb", "jpeg", &GIMG_PIXEL_RGBA12, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg cmyk", "jpeg", &GIMG_PIXEL_CMYK8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_cmyk_transform = 2;
    cases.push_back({"jpeg ycck", "jpeg", &GIMG_PIXEL_CMYK8, o, 0u, nullptr, 0u, 0u});
  }
  // PNG, BMP and GIF used to contribute four cases between them against
  // twenty-eight for JPEG, which is why the sweeps that walk this list reached
  // so much less of those three writers. Each block below names an axis the
  // writer branches on, not a variation for its own sake.
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"png rgba", "png", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.interlaced = 1;
    cases.push_back({"png interlaced rgba", "png", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    // Grayscale and sixteen-bit are separate writers, not the truecolor one
    // with a narrower pixel: PNG Table 11.1 gives each colour type its own
    // permitted depths and each depth its own row packing.
    GIMG_Save_Options o = opt();
    cases.push_back({"png gray8", "png", &GIMG_PIXEL_GRAY8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"png gray16", "png", &GIMG_PIXEL_GRAY16, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"png rgba16", "png", &GIMG_PIXEL_RGBA16, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.interlaced = 1;
    cases.push_back({"png interlaced gray16", "png", &GIMG_PIXEL_GRAY16, o, 0u,
        nullptr, 0u, 0u});
  }
  {
    // Few enough colours to build a palette from, which is a different writer
    // again - it quantizes nothing, it recognises that it does not have to.
    GIMG_Save_Options o = opt();
    cases.push_back({"png palette from few colours", "png", &GIMG_PIXEL_RGBA8,
        o, 4u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.png_palette = GIMG_PNG_PALETTE_NEVER;
    cases.push_back({"png truecolor forced", "png", &GIMG_PIXEL_RGBA8, o, 4u,
        nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.interlaced = 1;
    cases.push_back({"png interlaced palette", "png", &GIMG_PIXEL_RGBA8, o, 4u,
        nullptr, 0u, 0u});
  }
  // The five fixed row filters and the adaptive default are six separate
  // paths through the filter loop, and only the default was ever taken here.
  {
    GIMG_Save_Options o = opt();
    o.png_filter = GIMG_PNG_FILTER_NONE;
    cases.push_back({"png filter none", "png", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.png_filter = GIMG_PNG_FILTER_SUB;
    cases.push_back({"png filter sub", "png", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.png_filter = GIMG_PNG_FILTER_UP;
    cases.push_back({"png filter up", "png", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.png_filter = GIMG_PNG_FILTER_AVERAGE;
    cases.push_back({"png filter average", "png", &GIMG_PIXEL_RGBA8, o, 0u,
        nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.png_filter = GIMG_PNG_FILTER_PAETH;
    cases.push_back({"png filter paeth", "png", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"bmp rgba", "bmp", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.bmp_top_down = 1;
    cases.push_back({"bmp top-down", "bmp", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"bmp gray8", "bmp", &GIMG_PIXEL_GRAY8, o, 0u, nullptr, 0u, 0u});
  }
  {
    // An indexed bitmap, and then the three compressed forms of one. Each is
    // a separate encoder: BI_RLE8 and BI_RLE4 differ in how a run is packed,
    // RLE24 is an OS/2 extension, and the Huffman form is CCITT G3 1-D.
    GIMG_Save_Options o = opt();
    cases.push_back({"bmp palette", "bmp", &GIMG_PIXEL_RGBA8, o, 4u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.bmp_rle = GIMG_BMP_RLE_AUTO;
    cases.push_back({"bmp rle", "bmp", &GIMG_PIXEL_RGBA8, o, 4u, nullptr, 8u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.bmp_rle = GIMG_BMP_RLE_AUTO;
    o.bmp_allow_rle24 = 1;
    cases.push_back({"bmp rle24", "bmp", &GIMG_PIXEL_RGBA8, o, 16u, nullptr, 4u, 64u});
  }
  {
    GIMG_Save_Options o = opt();
    o.bmp_rle = GIMG_BMP_RLE_AUTO;
    o.bmp_allow_huffman = 1;
    cases.push_back({"bmp huffman", "bmp", &GIMG_PIXEL_GRAY8, o, 2u, nullptr, 8u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.bmp_rle = GIMG_BMP_RLE_AUTO;
    o.bmp_allow_2bit = 1;
    cases.push_back({"bmp 2-bit", "bmp", &GIMG_PIXEL_GRAY8, o, 4u, nullptr, 8u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.bmp_palette = GIMG_BMP_PALETTE_NEVER;
    cases.push_back({"bmp truecolor forced", "bmp", &GIMG_PIXEL_RGBA8, o, 4u,
        nullptr, 0u, 0u});
  }
  {
    // The wrapper forms write another codec's whole stream into a BMP, so the
    // BMP writer's own pixel path is not taken at all.
    GIMG_Save_Options o = opt();
    o.bmp_wrapper = GIMG_BMP_WRAPPER_PNG;
    cases.push_back({"bmp png wrapper", "bmp", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.bmp_wrapper = GIMG_BMP_WRAPPER_JPEG;
    cases.push_back({"bmp jpeg wrapper", "bmp", &GIMG_PIXEL_RGBA8, o, 0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    // GIF takes an RGBA8 raster and nothing else, and only one whose
    // colours already fit a table: 6 levels per channel is 216 of them.
    cases.push_back({"gif rgba 216 colours", "gif", &GIMG_PIXEL_RGBA8, o, 6u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.gif_interlace = 1;
    cases.push_back({"gif interlaced", "gif", &GIMG_PIXEL_RGBA8, o, 6u, nullptr, 0u, 0u});
  }
  {
    // A threshold above zero splits the table into opaque entries plus one
    // transparent index, which is a different plan from the opaque case.
    GIMG_Save_Options o = opt();
    o.gif_alpha_threshold = 128;
    cases.push_back({"gif transparency", "gif", &GIMG_PIXEL_RGBA8, o, 6u,
        nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.gif_loop_count = 3;
    cases.push_back({"gif loop count", "gif", &GIMG_PIXEL_RGBA8, o, 6u, nullptr, 0u, 0u});
  }

  {
    GIMG_Save_Options o = opt();
    o.jpeg_progressive = 1;
    o.jpeg_progressive_config = simple_progression();
    cases.push_back({"jpeg successive approx", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u,
        nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_progressive = 1;
    o.jpeg_progressive_config = simple_progression();
    o.jpeg_restart_interval = 4;
    cases.push_back({"jpeg successive + restarts", "jpeg", &GIMG_PIXEL_RGBA8, o,
        0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_progressive = 1;
    o.jpeg_progressive_config = simple_progression();
    o.jpeg_arithmetic = 1;
    cases.push_back({"jpeg successive arithmetic", "jpeg", &GIMG_PIXEL_RGBA8, o,
        0u, nullptr, 0u, 0u});
  }
  {
    // The twelve-bit scan writer is a separate function with its own
    // alphabets, so the eight-bit cases say nothing about it.
    GIMG_Save_Options o = opt();
    o.jpeg_progressive = 1;
    o.jpeg_progressive_config = simple_progression();
    o.jpeg_precision = 12;
    cases.push_back({"jpeg successive 12-bit", "jpeg", &GIMG_PIXEL_RGBA12, o,
        0u, nullptr, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg every app segment", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u,
        attach_jpeg_app_segments, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back(
        {"jpeg exif", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u, attach_exif, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg synthesized icc", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u,
        tag_adobe_rgb, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg description and dpi", "jpeg", &GIMG_PIXEL_RGBA8, o,
        0u, attach_common, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"png exif", "png", &GIMG_PIXEL_RGBA8, o, 0u, attach_exif, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"png description and dpi", "png", &GIMG_PIXEL_RGBA8, o, 0u,
        attach_common, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"png synthesized icc", "png", &GIMG_PIXEL_RGBA8, o, 0u,
        tag_adobe_rgb, 0u, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"gif description", "gif", &GIMG_PIXEL_RGBA8, o, 6u,
        attach_common, 0u, 0u});
  }
  return cases;
}

} // namespace gimg_test

#endif
