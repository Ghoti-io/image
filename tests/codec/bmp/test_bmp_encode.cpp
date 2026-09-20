/**
 * @file
 *
 * BMP save tests: header layout, depth selection, and round-trip fidelity.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

#include "bmp_test_utils.h"

using bmp_test::Loaded;
using bmp_test::Rgba;

namespace {

/** Read a little-endian 16-bit field from a saved file. */
uint16_t read_u16(const std::vector<uint8_t> & b, size_t at) {
  return (uint16_t)((uint32_t)b[at] | ((uint32_t)b[at + 1] << 8));
}

/** Read a little-endian 32-bit field from a saved file. */
uint32_t read_u32(const std::vector<uint8_t> & b, size_t at) {
  return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) |
      ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
}

/**
 * Build an RGBA8 raster from a caller-supplied pixel function.
 *
 * The caller owns the returned raster.
 */
GIMG_Raster * make_raster(uint32_t width, uint32_t height,
    Rgba (*pixel)(uint32_t, uint32_t)) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(width, height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
          nullptr, 0, &raster) != GIMG_OK) {
    return nullptr;
  }
  uint8_t * pixels = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      Rgba p = pixel(x, y);
      uint8_t * px = pixels + (y * stride) + (x * 4u);
      px[0] = p.r;
      px[1] = p.g;
      px[2] = p.b;
      px[3] = p.a;
    }
  }
  return raster;
}

/** Save a raster as BMP and return the bytes. */
GIMG_Result save_raster(GIMG_Raster * raster, std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_create(&doc);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_doc_set_item_count(doc, 1);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  // The document takes ownership of the raster.
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * stream = nullptr;
  r = gimg_stream_create_memory_output(&stream);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }

  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, stream, "bmp", nullptr, &report);
  if (r == GIMG_OK) {
    const void * buffer = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &buffer, &size);
    const uint8_t * bytes = static_cast<const uint8_t *>(buffer);
    out.assign(bytes, bytes + size);
    EXPECT_EQ(report.bytes_written, size)
        << "report must match what was actually written";
  }

  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r;
}

/** Save a raster as BMP with a resolution stated on the document. */
GIMG_Result save_raster_with_dpi(GIMG_Raster * raster, uint32_t x_dpi,
    uint32_t y_dpi, const GIMG_Save_Options * options,
    std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_create(&doc);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_doc_set_item_count(doc, 1);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Meta_Common * meta = nullptr;
  r = gimg_doc_ensure_meta_common(doc, &meta);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  gimg_meta_common_set_dpi(meta, x_dpi, y_dpi);

  GIMG_Stream * stream = nullptr;
  r = gimg_stream_create_memory_output(&stream);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, stream, "bmp", options, &report);
  if (r == GIMG_OK) {
    const void * buffer = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &buffer, &size);
    const uint8_t * bytes = static_cast<const uint8_t *>(buffer);
    out.assign(bytes, bytes + size);
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r;
}

Rgba opaque_gradient(uint32_t x, uint32_t y) {
  return Rgba{(uint8_t)(x * 17u), (uint8_t)(y * 23u),
      (uint8_t)((x + y) * 11u), 255};
}

Rgba alpha_gradient(uint32_t x, uint32_t y) {
  return Rgba{(uint8_t)(x * 17u), (uint8_t)(y * 23u),
      (uint8_t)((x + y) * 11u), (uint8_t)(x * 40u)};
}

} // namespace

TEST(BmpEncode, OpaqueRasterIsWrittenAs24Bit) {
  GIMG_Raster * raster = make_raster(5, 3, opaque_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  ASSERT_GE(bytes.size(), 54u);

  EXPECT_EQ(bytes[0], 'B');
  EXPECT_EQ(bytes[1], 'M');
  EXPECT_EQ(read_u32(bytes, 2), bytes.size()) << "bfSize must match the file";
  EXPECT_EQ(read_u32(bytes, 10), 54u) << "pixel data follows a 40-byte DIB";
  EXPECT_EQ(read_u32(bytes, 14), 40u) << "BITMAPINFOHEADER";
  EXPECT_EQ(read_u32(bytes, 18), 5u);
  EXPECT_EQ(read_u32(bytes, 22), 3u) << "positive height: rows are bottom-up";
  EXPECT_EQ(read_u16(bytes, 26), 1u) << "planes";
  EXPECT_EQ(read_u16(bytes, 28), 24u);
  EXPECT_EQ(read_u32(bytes, 30), 0u) << "BI_RGB";

  // 5 pixels * 3 bytes = 15, padded to 16 per row.
  EXPECT_EQ(read_u32(bytes, 34), 16u * 3u) << "biSizeImage";
  EXPECT_EQ(bytes.size(), 54u + (16u * 3u));
}

TEST(BmpEncode, RasterWithAlphaIsWrittenAs32BitBitfields) {
  GIMG_Raster * raster = make_raster(4, 2, alpha_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  EXPECT_EQ(read_u32(bytes, 14), 56u) << "BITMAPV3INFOHEADER carries the masks";
  EXPECT_EQ(read_u16(bytes, 28), 32u);
  EXPECT_EQ(read_u32(bytes, 30), 3u) << "BI_BITFIELDS";
  EXPECT_EQ(read_u32(bytes, 54), 0x00FF0000u) << "red mask";
  EXPECT_EQ(read_u32(bytes, 58), 0x0000FF00u) << "green mask";
  EXPECT_EQ(read_u32(bytes, 62), 0x000000FFu) << "blue mask";
  EXPECT_EQ(read_u32(bytes, 66), 0xFF000000u) << "alpha mask";
}

TEST(BmpEncode, OpaqueRoundTripIsExact) {
  // 5 is deliberately not a multiple of 4, so the row padding is exercised in
  // both directions.
  GIMG_Raster * raster = make_raster(5, 3, opaque_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 5u);
  ASSERT_EQ(img.height(), 3u);

  for (uint32_t y = 0; y < 3; y++) {
    for (uint32_t x = 0; x < 5; x++) {
      EXPECT_EQ(img.at(x, y), opaque_gradient(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpEncode, AlphaRoundTripIsExact) {
  GIMG_Raster * raster = make_raster(4, 2, alpha_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      EXPECT_EQ(img.at(x, y), alpha_gradient(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpEncode, SinglePixelRoundTrip) {
  GIMG_Raster * raster = make_raster(1, 1, opaque_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.width(), 1u);
  EXPECT_EQ(img.height(), 1u);
  EXPECT_EQ(img.at(0, 0), opaque_gradient(0, 0));
}

TEST(BmpEncode, ReSavingALoadedFileReproducesItsPixels) {
  // Load a fixture, save it back out, and load the result: the pixels must
  // survive, which exercises decode and encode against each other rather than
  // against the same assumptions.
  Loaded original;
  ASSERT_EQ(original.load("bmp_4x4_24bit.bmp"), GIMG_OK);
  ASSERT_EQ(original.decode(), GIMG_OK);

  GIMG_Raster * copy = nullptr;
  ASSERT_EQ(gimg_raster_copy(original.raster(), &copy), GIMG_OK);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(copy, bytes), GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(again.decode(), GIMG_OK);

  ASSERT_EQ(again.width(), original.width());
  ASSERT_EQ(again.height(), original.height());
  for (uint32_t y = 0; y < original.height(); y++) {
    for (uint32_t x = 0; x < original.width(); x++) {
      EXPECT_EQ(again.at(x, y), original.at(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpEncode, PaletteFixtureSurvivesASaveLoadCycle) {
  // An indexed source becomes RGBA on decode and 24-bit on save; the visible
  // pixels must still match.
  Loaded original;
  ASSERT_EQ(original.load("bmp_8x2_8bit.bmp"), GIMG_OK);
  ASSERT_EQ(original.decode(), GIMG_OK);

  GIMG_Raster * copy = nullptr;
  ASSERT_EQ(gimg_raster_copy(original.raster(), &copy), GIMG_OK);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(copy, bytes), GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(again.decode(), GIMG_OK);

  for (uint32_t y = 0; y < original.height(); y++) {
    for (uint32_t x = 0; x < original.width(); x++) {
      EXPECT_EQ(again.at(x, y), original.at(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---------------------------------------------------------------------------
// Physical resolution
// ---------------------------------------------------------------------------

TEST(BmpEncode, StatesNoResolutionWhenTheDocumentHasNone) {
  // biXPelsPerMeter of zero means "not stated", which is legal and is what
  // most writers emit.  This encoder used to write a fixed 2835 - 72 dpi -
  // into every image, which is a claim about the picture that nothing in it
  // supported.
  GIMG_Raster * raster = make_raster(4, 2, opaque_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(read_u32(bytes, 38), 0u) << "biXPelsPerMeter";
  EXPECT_EQ(read_u32(bytes, 42), 0u) << "biYPelsPerMeter";
}

TEST(BmpEncode, WritesTheResolutionTheDocumentCarries) {
  GIMG_Raster * raster = make_raster(4, 2, opaque_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster_with_dpi(raster, 300, 300, nullptr, bytes), GIMG_OK);
  // An inch is exactly 0.0254 m, so 300 dpi is 300 * 5000 / 127 = 11811 ppm.
  EXPECT_EQ(read_u32(bytes, 38), 11811u) << "biXPelsPerMeter";
  EXPECT_EQ(read_u32(bytes, 42), 11811u) << "biYPelsPerMeter";
}

TEST(BmpEncode, DropAllMetadataDropsTheResolutionToo) {
  GIMG_Raster * raster = make_raster(4, 2, opaque_gradient);
  ASSERT_NE(raster, nullptr);

  GIMG_Save_Options options = {};
  options.metadata_policy = GIMG_META_DROP_ALL;

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster_with_dpi(raster, 300, 300, &options, bytes), GIMG_OK);
  EXPECT_EQ(read_u32(bytes, 38), 0u) << "biXPelsPerMeter";
  EXPECT_EQ(read_u32(bytes, 42), 0u) << "biYPelsPerMeter";
}

TEST(BmpEncode, ResolutionSurvivesASaveAndLoad) {
  // Every ordinary resolution survives exactly: the conversion is 5000/127
  // and back in integer arithmetic, with rounding at each end.
  for (uint32_t dpi : {1u, 72u, 96u, 150u, 200u, 300u, 600u, 1200u}) {
    GIMG_Raster * raster = make_raster(4, 2, opaque_gradient);
    ASSERT_NE(raster, nullptr);

    std::vector<uint8_t> bytes;
    ASSERT_EQ(save_raster_with_dpi(raster, dpi, dpi, nullptr, bytes), GIMG_OK);

    Loaded back;
    ASSERT_EQ(back.load_bytes(bytes), GIMG_OK) << "dpi " << dpi;
    GIMG_Meta_Common * meta = gimg_doc_meta_common(back.doc());
    ASSERT_NE(meta, nullptr) << "dpi " << dpi;
    uint32_t x_dpi = 0, y_dpi = 0;
    gimg_meta_common_dpi(meta, &x_dpi, &y_dpi);
    EXPECT_EQ(x_dpi, dpi);
    EXPECT_EQ(y_dpi, dpi);
  }
}
