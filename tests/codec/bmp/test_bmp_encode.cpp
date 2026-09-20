/**
 * @file
 *
 * BMP save tests: header layout, depth selection, and round-trip fidelity.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <filesystem>
#include <fstream>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

#include "bmp_test_utils.h"

extern "C" {
#include "bmp_internal.h"
}

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

/**
 * Leave a saved file in GIMG_TEST_OUT_BMP together with the pixels it was
 * meant to hold, for tests/data/bmp/verify_bmp_output.py to read back with a
 * decoder that is not ours.
 *
 * Our decoder agreeing with our encoder proves nothing about either: a
 * channel swap, a row flip or a stride error that both halves share reads as
 * success from the inside.  The sidecar is top-down RGBA8, four bytes per
 * pixel, which is what every decoder can be asked to produce.
 */
void publish_for_verification(const char * name,
    const std::vector<uint8_t> & bytes, uint32_t width, uint32_t height,
    Rgba (*pixel)(uint32_t, uint32_t)) {
  const std::string dir = GIMG_TEST_OUT_BMP;
  const std::string path = dir + "/" + name;
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out) << "cannot write " << path;
  out.write(reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  out.close();

  std::ofstream expected(path + ".expected.rgba", std::ios::binary);
  ASSERT_TRUE(expected) << "cannot write the expectation beside " << path;
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      Rgba p = pixel(x, y);
      const char rgba[4] = {(char)p.r, (char)p.g, (char)p.b, (char)p.a};
      expected.write(rgba, 4);
    }
  }
}

Rgba alpha_gradient(uint32_t x, uint32_t y) {
  return Rgba{(uint8_t)(x * 17u), (uint8_t)(y * 23u),
      (uint8_t)((x + y) * 11u), (uint8_t)(x * 40u)};
}

/** Two colors: the fewest a palette can hold, so 1 bit per pixel. */
Rgba two_colors(uint32_t x, uint32_t y) {
  return ((x + y) % 2u) ? Rgba{255, 255, 255, 255} : Rgba{16, 32, 48, 255};
}

/** Twelve colors, which needs 4 bits per pixel. */
Rgba twelve_colors(uint32_t x, uint32_t y) {
  uint8_t n = (uint8_t)(((x / 4u) + y) % 12u);
  return Rgba{(uint8_t)(n * 21u), (uint8_t)(255u - n * 17u),
      (uint8_t)(n * 9u), 255};
}

/** Thirty-two colors in runs of eight: 8 bits per pixel, and compressible. */
Rgba thirty_two_colors_in_runs(uint32_t x, uint32_t y) {
  (void)y;
  uint8_t n = (uint8_t)((x / 8u) % 32u);
  return Rgba{(uint8_t)(n * 8u), (uint8_t)(n * 3u), (uint8_t)(n * 5u), 255};
}

/**
 * Two hundred pixels with no repeat, then a long run of one.
 *
 * The RLE8 encoder's absolute-run branch only runs for a stretch with no run
 * worth encoding in it, and the run-heavy fixture never reaches it: every row
 * there is runs from end to end.  This one exercises the absolute run, the
 * odd-length padding it needs (199 literals), and the lookahead that breaks
 * out of gathering literals when a run of three starts.
 */
Rgba literals_then_a_run(uint32_t x, uint32_t y) {
  (void)y;
  uint8_t n = (uint8_t)((x < 200u ? x : 199u) % 32u);
  return Rgba{(uint8_t)(n * 8u), (uint8_t)(n * 3u), (uint8_t)(n * 5u), 255};
}

/** More distinct colors than a palette can hold. */
Rgba too_many_colors(uint32_t x, uint32_t y) {
  uint32_t i = (y * 32u) + x;
  return Rgba{(uint8_t)(i & 0xFFu), (uint8_t)((i >> 8) & 0xFFu),
      (uint8_t)(i >> 4), 255};
}

/** Save with options, so the writer's choices can be asked for by name. */
GIMG_Result save_raster_with_options(GIMG_Raster * raster,
    const GIMG_Save_Options * options, std::vector<uint8_t> & out) {
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
    EXPECT_EQ(report.bytes_written, size)
        << "report must match what was actually written";
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r;
}

/** Load a saved file back and require every pixel to be what went in. */
void expect_round_trip(const std::vector<uint8_t> & bytes, uint32_t width,
    uint32_t height, Rgba (*pixel)(uint32_t, uint32_t)) {
  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), width);
  ASSERT_EQ(img.height(), height);
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      EXPECT_EQ(img.at(x, y), pixel(x, y)) << "at (" << x << "," << y << ")";
    }
  }
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

  publish_for_verification("opaque_24bit_5x3.bmp", bytes, 5, 3,
      opaque_gradient);
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

  publish_for_verification("alpha_32bit_4x2.bmp", bytes, 4, 2, alpha_gradient);
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

// ---------------------------------------------------------------------------
// Indexed output
// ---------------------------------------------------------------------------

TEST(BmpEncode, TwoColorImageIsWrittenAtOneBitPerPixel) {
  // With 256 colors or fewer there is exactly one palette that reproduces the
  // image, so storing it through one decides nothing about the picture.  Two
  // colors need one bit, and at 64 wide that is 8 bytes a row against 192.
  GIMG_Raster * raster = make_raster(64, 8, two_colors);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  EXPECT_EQ(read_u16(bytes, 28), 1u) << "biBitCount";
  EXPECT_EQ(read_u32(bytes, 30), 0u) << "BI_RGB";
  EXPECT_EQ(read_u32(bytes, 46), 2u) << "biClrUsed";
  EXPECT_EQ(read_u32(bytes, 10), 54u + (2u * 4u)) << "palette precedes pixels";

  expect_round_trip(bytes, 64, 8, two_colors);
  publish_for_verification("indexed_1bit_64x8.bmp", bytes, 64, 8, two_colors);
}

TEST(BmpEncode, TwelveColorImageIsWrittenAtFourBitsPerPixel) {
  GIMG_Raster * raster = make_raster(64, 8, twelve_colors);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  EXPECT_EQ(read_u16(bytes, 28), 4u) << "biBitCount";
  EXPECT_EQ(read_u32(bytes, 46), 12u) << "biClrUsed";

  expect_round_trip(bytes, 64, 8, twelve_colors);
  publish_for_verification("indexed_4bit_64x8.bmp", bytes, 64, 8,
      twelve_colors);
}

TEST(BmpEncode, ThirtyTwoColorImageIsWrittenAtEightBitsPerPixel) {
  GIMG_Raster * raster = make_raster(256, 8, thirty_two_colors_in_runs);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  EXPECT_EQ(read_u16(bytes, 28), 8u) << "biBitCount";
  EXPECT_EQ(read_u32(bytes, 46), 32u) << "biClrUsed";

  expect_round_trip(bytes, 256, 8, thirty_two_colors_in_runs);
  publish_for_verification("indexed_8bit_256x8.bmp", bytes, 256, 8,
      thirty_two_colors_in_runs);
}

TEST(BmpEncode, TooManyColorsStaysTrueColor) {
  // Reducing an image to 256 colors would be color quantization, which is an
  // image-processing decision and not a codec's.
  GIMG_Raster * raster = make_raster(32, 32, too_many_colors);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(read_u16(bytes, 28), 24u) << "biBitCount";
  EXPECT_EQ(read_u32(bytes, 46), 0u) << "biClrUsed";
  expect_round_trip(bytes, 32, 32, too_many_colors);
}

TEST(BmpEncode, PaletteNeverKeepsTrueColor) {
  GIMG_Save_Options options = {};
  options.bmp_palette = GIMG_BMP_PALETTE_NEVER;

  GIMG_Raster * raster = make_raster(64, 8, two_colors);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster_with_options(raster, &options, bytes), GIMG_OK);
  EXPECT_EQ(read_u16(bytes, 28), 24u) << "biBitCount";
  expect_round_trip(bytes, 64, 8, two_colors);
}

TEST(BmpEncode, TransparencyRulesOutThePalette) {
  // A BMP palette has no alpha, so an image that needs one cannot use it
  // however few colors it has.
  GIMG_Raster * raster = make_raster(4, 2, alpha_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(read_u16(bytes, 28), 32u) << "biBitCount";
  EXPECT_EQ(read_u32(bytes, 46), 0u) << "biClrUsed";
}

TEST(BmpEncode, PaletteIsNotUsedWhenItWouldBeLarger) {
  // Four pixels of four colors.  At 4 bits each the rows come to 4 bytes and
  // the palette to 16, against 12 bytes of plain 24-bit colour - so storing
  // it through a palette would make the file larger, and the arithmetic has
  // to come out against it.
  GIMG_Raster * raster = make_raster(4, 1, too_many_colors);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(read_u16(bytes, 28), 24u) << "biBitCount";
}

// ---------------------------------------------------------------------------
// RLE8 output
// ---------------------------------------------------------------------------

TEST(BmpEncode, RleIsNotWrittenUnlessAskedFor) {
  // An uncompressed BMP is the most widely readable image there is, which is
  // most of why the format is still worth writing, so compression is opt-in.
  GIMG_Raster * raster = make_raster(256, 8, thirty_two_colors_in_runs);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(read_u32(bytes, 30), 0u) << "BI_RGB";
}

TEST(BmpEncode, RleAutoWritesRle8ForARunHeavyImage) {
  GIMG_Save_Options options = {};
  options.bmp_rle = GIMG_BMP_RLE_AUTO;

  GIMG_Raster * plain = make_raster(256, 8, thirty_two_colors_in_runs);
  GIMG_Raster * raster = make_raster(256, 8, thirty_two_colors_in_runs);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> uncompressed;
  ASSERT_EQ(save_raster(plain, uncompressed), GIMG_OK);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster_with_options(raster, &options, bytes), GIMG_OK);

  EXPECT_EQ(read_u16(bytes, 28), 8u) << "biBitCount";
  EXPECT_EQ(read_u32(bytes, 30), 1u) << "BI_RLE8";
  EXPECT_LT(bytes.size(), uncompressed.size())
      << "the encoded form is only chosen when it is smaller";
  EXPECT_EQ(read_u32(bytes, 34), bytes.size() - read_u32(bytes, 10))
      << "biSizeImage must state the encoded length";

  expect_round_trip(bytes, 256, 8, thirty_two_colors_in_runs);
  publish_for_verification("rle8_256x8.bmp", bytes, 256, 8,
      thirty_two_colors_in_runs);
}

TEST(BmpEncode, RleIsDeclinedWhenItWouldNotHelp) {
  // A checkerboard at one bit per pixel is not an 8-bit indexed image, so
  // there is nothing for BI_RLE8 to apply to and the request is simply not
  // taken up - not refused.
  GIMG_Save_Options options = {};
  options.bmp_rle = GIMG_BMP_RLE_AUTO;

  GIMG_Raster * raster = make_raster(64, 8, two_colors);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster_with_options(raster, &options, bytes), GIMG_OK);
  EXPECT_EQ(read_u32(bytes, 30), 0u) << "BI_RGB";
  expect_round_trip(bytes, 64, 8, two_colors);
}

// ---------------------------------------------------------------------------
// Row order
// ---------------------------------------------------------------------------

TEST(BmpEncode, TopDownWritesANegativeHeight) {
  GIMG_Save_Options options = {};
  options.bmp_top_down = 1;

  GIMG_Raster * raster = make_raster(5, 3, opaque_gradient);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster_with_options(raster, &options, bytes), GIMG_OK);

  int32_t height = (int32_t)read_u32(bytes, 22);
  EXPECT_EQ(height, -3) << "a negative biHeight means top-down rows";

  expect_round_trip(bytes, 5, 3, opaque_gradient);
  publish_for_verification("topdown_24bit_5x3.bmp", bytes, 5, 3,
      opaque_gradient);
}

TEST(BmpEncode, TopDownAndRleTogetherAreRefused) {
  // The format does not allow the pair: an RLE stream's end-of-line walks one
  // way only, so a top-down RLE bitmap does not say which way it walks.  The
  // loader here refuses such a file, and writing one would be producing
  // something this library will not read back.
  GIMG_Save_Options options = {};
  options.bmp_top_down = 1;
  options.bmp_rle = GIMG_BMP_RLE_AUTO;

  GIMG_Raster * raster = make_raster(64, 8, two_colors);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  EXPECT_EQ(save_raster_with_options(raster, &options, bytes),
      GIMG_ERR_UNSUPPORTED);
}

TEST(BmpEncode, Rle8EncodesAbsoluteRunsAsWellAsRepeats) {
  // The run-heavy fixture never reaches the encoder's absolute-run branch,
  // because every row of it is runs from end to end.  This image is 200
  // pixels with no repeat followed by a run of one colour, so it takes the
  // absolute run, the odd-length padding that needs, and the lookahead that
  // stops gathering literals when a run of three begins.
  GIMG_Save_Options options = {};
  options.bmp_rle = GIMG_BMP_RLE_AUTO;

  GIMG_Raster * plain = make_raster(256, 8, literals_then_a_run);
  GIMG_Raster * raster = make_raster(256, 8, literals_then_a_run);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> uncompressed;
  ASSERT_EQ(save_raster(plain, uncompressed), GIMG_OK);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster_with_options(raster, &options, bytes), GIMG_OK);

  EXPECT_EQ(read_u32(bytes, 30), 1u) << "BI_RLE8";
  EXPECT_LT(bytes.size(), uncompressed.size());

  expect_round_trip(bytes, 256, 8, literals_then_a_run);
  publish_for_verification("rle8_absolute_256x8.bmp", bytes, 256, 8,
      literals_then_a_run);
}

// ---------------------------------------------------------------------------
// Every fixture, every combination of the writer's options
// ---------------------------------------------------------------------------

TEST(BmpEncode, EveryFixtureSurvivesEveryCombinationOfTheWritersOptions) {
  // The tests above each pin one choice the writer makes.  This one asserts
  // the invariant that ties them together: whatever the writer produces, from
  // whatever this codec was able to read, must load back to the same picture -
  // for all eight combinations of palette, RLE and row order.
  //
  // It walks the fixture directory rather than a list, so a fixture added for
  // some other reason is covered by this the moment it lands.
  //
  // Two outcomes are not failures.  A fixture that does not load is one of the
  // deliberately malformed ones, and a save that returns UNSUPPORTED is either
  // the top-down-plus-RLE pair the format forbids or a raster whose format the
  // writer does not take.
  namespace fs = std::filesystem;
  size_t round_trips = 0;
  size_t fixtures = 0;

  for (const auto & entry : fs::directory_iterator(GIMG_TEST_DATA_BMP)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".bmp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    fixtures++;

    Loaded original;
    if (original.load(name.c_str()) != GIMG_OK) {
      continue;  // A deliberately malformed fixture.
    }
    if (original.decode() != GIMG_OK) {
      continue;
    }
    const uint32_t width = original.width();
    const uint32_t height = original.height();

    for (int palette = 0; palette <= 1; palette++) {
      for (int rle = 0; rle <= 1; rle++) {
        for (int top_down = 0; top_down <= 1; top_down++) {
          GIMG_Save_Options options = {};
          options.bmp_palette = (uint8_t)palette;
          options.bmp_rle = (uint8_t)rle;
          options.bmp_top_down = (uint8_t)top_down;

          // The document owns the raster it is given, so each pass needs its
          // own copy of the picture rather than the one `original` holds.
          GIMG_Raster * copy = nullptr;
          ASSERT_EQ(gimg_raster_copy(original.raster(), &copy), GIMG_OK)
              << name;

          std::vector<uint8_t> bytes;
          GIMG_Result r = save_raster_with_options(copy, &options, bytes);
          if (r == GIMG_ERR_UNSUPPORTED) {
            // Either the pair the format forbids, or a raster format the
            // writer does not take.  Both are documented refusals.
            continue;
          }
          ASSERT_EQ(r, GIMG_OK)
              << name << " palette=" << palette << " rle=" << rle
              << " top_down=" << top_down;

          Loaded back;
          ASSERT_EQ(back.load_bytes(bytes), GIMG_OK)
              << name << ": what the writer produced would not load back"
              << " (palette=" << palette << " rle=" << rle
              << " top_down=" << top_down << ")";
          ASSERT_EQ(back.decode(), GIMG_OK) << name;
          ASSERT_EQ(back.width(), width) << name;
          ASSERT_EQ(back.height(), height) << name;

          for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
              ASSERT_EQ(back.at(x, y), original.at(x, y))
                  << name << " at (" << x << "," << y << ")"
                  << " palette=" << palette << " rle=" << rle
                  << " top_down=" << top_down;
            }
          }
          round_trips++;
        }
      }
    }
  }

  EXPECT_GT(fixtures, 30u) << "the fixture directory should not be nearly empty";
  EXPECT_GT(round_trips, 100u) << "most fixtures should have round-tripped";
}

namespace {

/**
 * A raster of @p bits-per-channel samples, filled from a 16-bit pixel
 * function whose values are narrowed to the raster's own depth.
 */
GIMG_Raster * make_deep_raster(uint32_t width, uint32_t height,
    const GIMG_Pixel_Format * fmt, const std::vector<uint16_t> & samples) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(
          width, height, fmt, GIMG_RASTER_OWNED, nullptr, 0, &raster) !=
      GIMG_OK) {
    return nullptr;
  }
  uint8_t * pixels = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  size_t stride = gimg_raster_stride_bytes(raster);
  size_t channels = fmt->channel_count;
  size_t i = 0;
  for (uint32_t y = 0; y < height; y++) {
    uint16_t * row = reinterpret_cast<uint16_t *>(pixels + (y * stride));
    for (uint32_t x = 0; x < width * channels; x++) {
      row[x] = samples[i % samples.size()];
      i++;
    }
  }
  return raster;
}

/** round(v * 255 / 65535), the rule gimg_bitdepth_16_to_8 applies. */
uint8_t narrow_16_to_8(uint16_t v) {
  return (uint8_t)((((uint32_t)v * 255u) + 32767u) / 65535u);
}

} // namespace

TEST(BmpEncode, A16BitRasterIsNarrowedRatherThanRefused) {
  // A BMP sample is a byte at most, so a deeper raster is restated at 8 bits.
  // This used to return GIMG_ERR_UNSUPPORTED, which meant a 16-bit PNG could
  // not be saved as a BMP at all.
  std::vector<uint16_t> samples = {0u, 1u, 255u, 256u, 13107u, 32768u,
      65534u, 65535u, 26214u, 4096u, 900u, 60000u};
  GIMG_Raster * raster =
      make_deep_raster(3, 2, &GIMG_PIXEL_RGBA16, samples);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 3u);
  ASSERT_EQ(img.height(), 2u);

  size_t i = 0;
  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 3; x++) {
      Rgba want{narrow_16_to_8(samples[(i + 0) % samples.size()]),
          narrow_16_to_8(samples[(i + 1) % samples.size()]),
          narrow_16_to_8(samples[(i + 2) % samples.size()]),
          narrow_16_to_8(samples[(i + 3) % samples.size()])};
      EXPECT_EQ(img.at(x, y), want) << "at (" << x << "," << y << ")";
      i += 4;
    }
  }
}

TEST(BmpEncode, A16BitGrayRasterIsNarrowedToo) {
  std::vector<uint16_t> samples = {0u, 32768u, 65535u, 13107u};
  GIMG_Raster * raster =
      make_deep_raster(4, 1, &GIMG_PIXEL_GRAY16, samples);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t x = 0; x < 4; x++) {
    uint8_t v = narrow_16_to_8(samples[x]);
    EXPECT_EQ(img.at(x, 0), (Rgba{v, v, v, 255u})) << "at x=" << x;
  }
}

TEST(BmpEncode, A12BitRasterIsNarrowedToo) {
  // 12-bit samples are the ones a JPEG at extended precision decodes to.
  std::vector<uint16_t> samples = {0u, 2048u, 4095u, 1024u};
  GIMG_Raster * raster =
      make_deep_raster(4, 1, &GIMG_PIXEL_GRAY12, samples);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t x = 0; x < 4; x++) {
    uint16_t v12 = samples[x];
    uint8_t v = v12 >= 4095u
        ? 255u
        : (uint8_t)((((uint32_t)v12 * 255u) + 2047u) / 4095u);
    EXPECT_EQ(img.at(x, 0), (Rgba{v, v, v, 255u})) << "at x=" << x;
  }
}

TEST(BmpEncode, NarrowingKeepsTheProfileTheDeepRasterCarried) {
  // The narrowed raster is the one the writer goes on to inspect, so a
  // conversion that lost the color info would lose it here even once the
  // writer learns to state one.
  std::vector<uint16_t> samples = {0u, 32768u, 65535u, 13107u};
  GIMG_Raster * raster =
      make_deep_raster(4, 1, &GIMG_PIXEL_GRAY16, samples);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> profile(128, 0);
  profile[3] = 128;
  std::memcpy(profile.data() + 36, "acsp", 4);
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.icc_bytes = profile.data();
  ci.icc_size = profile.size();
  ASSERT_EQ(gimg_raster_set_color_info(raster, &ci), GIMG_OK);

  GIMG_Raster * narrowed = nullptr;
  ASSERT_EQ(gimg_ops_convert_bit_depth(raster, 8, &narrowed), GIMG_OK);
  ASSERT_NE(narrowed, nullptr);
  const GIMG_Color_Info * got = gimg_raster_color_info_const(narrowed);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->icc_size, profile.size());
  gimg_raster_destroy(narrowed);

  std::vector<uint8_t> bytes;
  EXPECT_EQ(save_raster(raster, bytes), GIMG_OK);
}

namespace {

/** Save a raster as BMP under a metadata policy. */
GIMG_Result save_raster_with_policy(GIMG_Raster * raster,
    GIMG_Meta_Policy policy, std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_create(&doc);
  if (r != GIMG_OK) {
    return r;
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * stream = nullptr;
  r = gimg_stream_create_memory_output(&stream);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, stream, "bmp", &opts, &report);
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

/** An opaque 4x2 raster tagged with the given color. */
GIMG_Raster * colored_raster(const GIMG_Color_Info & color) {
  GIMG_Raster * raster = make_raster(4, 2, opaque_gradient);
  if (!raster) {
    return nullptr;
  }
  if (gimg_raster_set_color_info(raster, &color) != GIMG_OK) {
    gimg_raster_destroy(raster);
    return nullptr;
  }
  return raster;
}

/** The DIB header size a saved file declares. */
uint32_t dib_size_of(const std::vector<uint8_t> & bytes) {
  return read_u32(bytes, 14);
}

/** A field of the DIB header, by its offset within that header. */
uint32_t dib_u32(const std::vector<uint8_t> & bytes, size_t at) {
  return read_u32(bytes, 14 + at);
}

/** A 128-byte stand-in for an ICC profile. */
std::vector<uint8_t> small_profile() {
  std::vector<uint8_t> profile(128, 0);
  profile[3] = 128;
  std::memcpy(profile.data() + 36, "acsp", 4);
  for (size_t i = 40; i < profile.size(); i++) {
    profile[i] = (uint8_t)((i * 7u) & 0xFFu);
  }
  return profile;
}

} // namespace

TEST(BmpEncode, ARasterWithNothingToSayKeepsTheSmallestHeader) {
  // A V4 header costs 68 bytes, so it is written only when there is something
  // for it to carry.
  GIMG_Raster * raster = make_raster(4, 2, opaque_gradient);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(dib_size_of(bytes), 40u);
}

TEST(BmpEncode, AProfileOnTheRasterIsWrittenIntoAV5Header) {
  // Only a V5 header can locate an embedded profile, so a raster carrying one
  // gets that header and the profile follows the pixels.  Without this a BMP
  // loaded and saved as a BMP lost the profile it arrived with.
  std::vector<uint8_t> profile = small_profile();
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  ASSERT_EQ(dib_size_of(bytes), 124u);
  EXPECT_EQ(dib_u32(bytes, 56), 0x4D424544u) << "bV5CSType must be 'MBED'";

  uint32_t offset = dib_u32(bytes, 112);
  uint32_t size = dib_u32(bytes, 116);
  ASSERT_EQ(size, profile.size());
  // bV5ProfileData is measured from the start of the DIB header.
  size_t at = 14u + offset;
  ASSERT_LE(at + size, bytes.size());
  EXPECT_EQ(std::memcmp(bytes.data() + at, profile.data(), size), 0)
      << "the bytes at bV5ProfileData must be the profile";

  // And it comes back.
  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  const GIMG_Color_Info * back = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(back, nullptr);
  ASSERT_EQ(back->icc_size, profile.size());
  EXPECT_EQ(std::memcmp(back->icc_bytes, profile.data(), profile.size()), 0);
}

TEST(BmpEncode, AnSrgbRasterIsWrittenAsAV4HeaderNamingSrgb) {
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_SRGB;
  color.white_point = GIMG_PRIMARIES_SRGB;
  color.transfer = GIMG_TRANSFER_SRGB;
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(dib_size_of(bytes), 108u);
  EXPECT_EQ(dib_u32(bytes, 56), 0x73524742u) << "bV4CSType must be 'sRGB'";

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  const GIMG_Color_Info * back = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->transfer, GIMG_TRANSFER_SRGB);
  EXPECT_EQ(back->primaries, GIMG_PRIMARIES_SRGB);
}

TEST(BmpEncode, CalibratedPrimariesAndGammaSurviveTheHeader) {
  // bmpsuite's g/pal8v4.bmp is exactly this shape: sRGB's primaries with a
  // gamma of 2.2, which is not sRGB and must not be written as though it
  // were.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_ADOBE_RGB;
  color.white_point = GIMG_PRIMARIES_ADOBE_RGB;
  color.transfer = GIMG_TRANSFER_GAMMA;
  color.gamma_value = 2.2;
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  ASSERT_EQ(dib_size_of(bytes), 108u);
  EXPECT_EQ(dib_u32(bytes, 56), 0u) << "bV4CSType must be LCS_CALIBRATED_RGB";
  // 2.2 in 16.16 fixed point.
  EXPECT_EQ(dib_u32(bytes, 96), (uint32_t)(2.2 * 65536.0 + 0.5));
  EXPECT_EQ(dib_u32(bytes, 100), dib_u32(bytes, 96));
  EXPECT_EQ(dib_u32(bytes, 104), dib_u32(bytes, 96));

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  const GIMG_Color_Info * back = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->primaries, GIMG_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(back->transfer, GIMG_TRANSFER_GAMMA);
  EXPECT_NEAR(back->gamma_value, 2.2, 0.0001);
}

TEST(BmpEncode, LinearIsWrittenAsAGammaOfOne) {
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_LINEAR;
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  ASSERT_EQ(dib_size_of(bytes), 108u);
  EXPECT_EQ(dib_u32(bytes, 96), 65536u);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(gimg_raster_color_info_const(img.raster())->transfer,
      GIMG_TRANSFER_LINEAR);
}

TEST(BmpEncode, AnIntentOtherThanPerceptualNeedsAV5Header) {
  // bV5Intent is the only V5-only field this writer has anything to put in,
  // so it is what decides between the two header versions when there is no
  // profile.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_SRGB;
  color.intent = GIMG_INTENT_SATURATION;
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  ASSERT_EQ(dib_size_of(bytes), 124u);
  EXPECT_EQ(dib_u32(bytes, 108), 1u) << "LCS_GM_BUSINESS";
  EXPECT_EQ(dib_u32(bytes, 116), 0u) << "no profile, so no profile size";

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(gimg_raster_color_info_const(img.raster())->intent,
      GIMG_INTENT_SATURATION);
}

TEST(BmpEncode, AlphaMasksAndAProfileAreWrittenTogether) {
  // A V4 header carries the four channel masks at the same offsets a V3 does,
  // so stating a color space must not cost the alpha mask.
  std::vector<uint8_t> profile = small_profile();
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();
  GIMG_Raster * raster = make_raster(4, 2, alpha_gradient);
  ASSERT_NE(raster, nullptr);
  ASSERT_EQ(gimg_raster_set_color_info(raster, &color), GIMG_OK);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  ASSERT_EQ(dib_size_of(bytes), 124u);
  EXPECT_EQ(read_u16(bytes, 14 + 14), 32u) << "still 32 bits per pixel";
  EXPECT_EQ(dib_u32(bytes, 16), 3u) << "still BI_BITFIELDS";
  EXPECT_EQ(dib_u32(bytes, 40), 0x00FF0000u);
  EXPECT_EQ(dib_u32(bytes, 44), 0x0000FF00u);
  EXPECT_EQ(dib_u32(bytes, 48), 0x000000FFu);
  EXPECT_EQ(dib_u32(bytes, 52), 0xFF000000u);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      EXPECT_EQ(img.at(x, y), alpha_gradient(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
  EXPECT_EQ(gimg_raster_color_info_const(img.raster())->icc_size,
      profile.size());
}

TEST(BmpEncode, DropAllAndKeepRawOnlyStateNoColorSpace) {
  // The two policies that drop the resolution drop this too.
  std::vector<uint8_t> profile = small_profile();
  for (GIMG_Meta_Policy policy :
      {GIMG_META_DROP_ALL, GIMG_META_KEEP_RAW_ONLY}) {
    GIMG_Color_Info color;
    gimg_color_info_default(&color);
    color.icc_bytes = profile.data();
    color.icc_size = profile.size();
    GIMG_Raster * raster = colored_raster(color);
    ASSERT_NE(raster, nullptr);
    std::vector<uint8_t> bytes;
    ASSERT_EQ(save_raster_with_policy(raster, policy, bytes), GIMG_OK);
    EXPECT_EQ(dib_size_of(bytes), 40u) << "policy " << (int)policy;
  }
}

TEST(BmpEncode, AGammaTheFieldCannotHoldGoesUnsaid) {
  // bV4Gamma is 16.16 fixed point, so it states nothing above 65535.  A value
  // past that is left out rather than converted, which for the same reason as
  // PNG's gAMA would be undefined behaviour and not a large number.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_GAMMA;
  color.gamma_value = 1e9;
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(dib_size_of(bytes), 40u)
      << "nothing was left to say, so no V4 header";
}

TEST(BmpEncode, ProfileBearingFilesStillReadBackFromOutside) {
  // Left for tests/data/bmp/verify_bmp_output.py: a V5 header is longer than
  // any this writer used to emit, and a decoder that stops reading at 40
  // bytes would find the pixels in the wrong place.
  std::vector<uint8_t> profile = small_profile();
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();
  GIMG_Raster * raster = make_raster(5, 3, opaque_gradient);
  ASSERT_NE(raster, nullptr);
  ASSERT_EQ(gimg_raster_set_color_info(raster, &color), GIMG_OK);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  publish_for_verification("v5_profile.bmp", bytes, 5, 3, opaque_gradient);
}

TEST(BmpEncode, AProfilePastWhatThisCodecReadsIsNotEmbedded) {
  // bV5ProfileSize is 32 bits, so without a ceiling a file could name a
  // profile of four gigabytes.  The writer obeys the same ceiling the loader
  // does, so it never produces a file this codec would refuse to read whole -
  // and a raster whose only colour is an over-large profile gets the smallest
  // header, not a V5 one pointing at nothing.
  std::vector<uint8_t> profile(GIMG_BMP_ICC_MAX_SIZE + 1u, 0);
  profile[3] = 0;
  std::memcpy(profile.data() + 36, "acsp", 4);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  EXPECT_EQ(dib_size_of(bytes), 40u);
}

TEST(BmpEncode, AnOverLargeProfileDoesNotSuppressTheRestOfTheColor) {
  // The rendering intent is what pushes a header to V5 when there is no
  // profile, and it used to drag an over-large profile along with it.
  std::vector<uint8_t> profile(GIMG_BMP_ICC_MAX_SIZE + 1u, 0);
  std::memcpy(profile.data() + 36, "acsp", 4);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_SRGB;
  color.intent = GIMG_INTENT_SATURATION;
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, bytes), GIMG_OK);
  ASSERT_EQ(dib_size_of(bytes), 124u);
  EXPECT_EQ(dib_u32(bytes, 56), 0x73524742u) << "sRGB, not PROFILE_EMBEDDED";
  EXPECT_EQ(dib_u32(bytes, 116), 0u) << "and no profile size";
  EXPECT_LT(bytes.size(), 4096u) << "the profile must not have been written";

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  const GIMG_Color_Info * back = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->transfer, GIMG_TRANSFER_SRGB);
  EXPECT_EQ(back->intent, GIMG_INTENT_SATURATION);
  EXPECT_EQ(back->icc_size, 0u);
}

TEST(BmpEncode, SavingFromADocumentWhoseRasterTheSaveOwnsWritesTheRightProfile) {
  // The PNG and JPEG writers both read a raster they had already destroyed
  // when the save had decoded it for itself, and wrote freed memory into the
  // file.  This writer holds the raster to the end and so is not in that
  // state - which is worth an assertion rather than an argument.
  std::vector<uint8_t> profile = small_profile();
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();
  GIMG_Raster * raster = colored_raster(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> first;
  ASSERT_EQ(save_raster(raster, first), GIMG_OK);

  // Load it back and save again without attaching a raster, so the second
  // save decodes for itself and the profile it writes comes from a raster it
  // owns.
  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(first.data(), first.size(), &in),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_EQ(gimg_item_raster(gimg_doc_item(doc, 0)), nullptr)
      << "the save must decode for itself, or this tests nothing";

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "bmp", nullptr, &report), GIMG_OK);
  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &bytes, &size);
  std::vector<uint8_t> second(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);

  ASSERT_EQ(dib_size_of(second), 124u);
  uint32_t at = 14u + dib_u32(second, 112);
  uint32_t len = dib_u32(second, 116);
  ASSERT_EQ(len, profile.size());
  ASSERT_LE(at + len, second.size());
  EXPECT_EQ(std::memcmp(second.data() + at, profile.data(), len), 0)
      << "the profile written must be the one that went in";
  EXPECT_EQ(first, second) << "and the second save must reproduce the first";
}
