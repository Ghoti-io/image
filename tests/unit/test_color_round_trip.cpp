/**
 * @file
 *
 * Does what a file says about its samples survive a conversion?
 *
 * Two things travel this road: the color space and the physical resolution.
 * Both live on the document rather than in any one format's syntax, and both
 * are written by machinery each codec keeps to itself.
 *
 * Each codec carries an ICC profile in a different place - a BMP in a
 * BITMAPV5HEADER, a PNG in iCCP, a JPEG in APP2 segments - and each writer
 * used to read only from where its own format keeps it. A document that
 * arrived as one format and left as another had nothing to read, so the
 * profile was decoded and then dropped. Five of the nine pairs below lost it.
 *
 * This asserts the whole matrix at once, because the defect was never in one
 * writer: it was the same omission made three times, and a test per codec
 * would have been three chances to make it again.
 *
 * Pixels are not compared - JPEG is lossy and a palette may quantize - only
 * what the file says its samples mean.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

/** A profile big enough to be worth carrying and small enough to be quick. */
std::vector<uint8_t> a_profile() {
  std::vector<uint8_t> profile(1024, 0);
  profile[0] = 0;
  profile[1] = 0;
  profile[2] = 4;
  profile[3] = 0; // declared size 1024
  std::memcpy(profile.data() + 36, "acsp", 4);
  for (size_t i = 40; i < profile.size(); i++) {
    profile[i] = (uint8_t)((i * 31u) & 0xFFu);
  }
  return profile;
}

/** A 16x16 opaque RGBA8 raster tagged with @p profile and nothing else. */
GIMG_Raster * tagged(const std::vector<uint8_t> & profile) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(
          16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &raster) !=
          GIMG_OK ||
      !raster) {
    return nullptr;
  }
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      uint8_t * p = px + (y * stride) + (x * 4u);
      p[0] = (uint8_t)(x * 16u);
      p[1] = (uint8_t)(y * 16u);
      p[2] = 96u;
      p[3] = 255u;
    }
  }
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = profile.data();
  ci.icc_size = profile.size();
  if (gimg_raster_set_color_info(raster, &ci) != GIMG_OK) {
    gimg_raster_destroy(raster);
    return nullptr;
  }
  return raster;
}

/** Save a document (taking ownership of @p raster) as @p format. */
::testing::AssertionResult save_as(GIMG_Raster * raster, const char * format,
    std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK || !doc) {
    gimg_raster_destroy(raster);
    return ::testing::AssertionFailure() << "gimg_doc_create";
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * stream = nullptr;
  if (gimg_stream_create_memory_output(&stream) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return ::testing::AssertionFailure() << "gimg_stream_create_memory_output";
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, stream, format, &opts, &report);
  if (r == GIMG_OK) {
    const void * bytes = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &bytes, &size);
    out.assign(static_cast<const uint8_t *>(bytes),
        static_cast<const uint8_t *>(bytes) + size);
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r == GIMG_OK
      ? ::testing::AssertionSuccess()
      : ::testing::AssertionFailure() << "save as " << format << ": " << r;
}

/**
 * Load @p bytes, save the document as @p format without attaching a raster,
 * and return what was written.
 *
 * Not attaching one is the point: the writer has to decode for itself, which
 * is both the ordinary conversion case and the one where the raster does not
 * outlive the save.
 */
::testing::AssertionResult convert(const std::vector<uint8_t> & bytes,
    const char * format, std::vector<uint8_t> & out) {
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return ::testing::AssertionFailure() << "gimg_stream_create_memory";
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(in);
    return ::testing::AssertionFailure() << "load: " << r;
  }
  GIMG_Stream * stream = nullptr;
  if (gimg_stream_create_memory_output(&stream) != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
    return ::testing::AssertionFailure() << "gimg_stream_create_memory_output";
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, stream, format, &opts, &report);
  if (r == GIMG_OK) {
    const void * written = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &written, &size);
    out.assign(static_cast<const uint8_t *>(written),
        static_cast<const uint8_t *>(written) + size);
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  return r == GIMG_OK
      ? ::testing::AssertionSuccess()
      : ::testing::AssertionFailure() << "save as " << format << ": " << r;
}

/** The profile a file carries, as this library reads it back. */
::testing::AssertionResult profile_of(
    const std::vector<uint8_t> & bytes, std::vector<uint8_t> & out) {
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return ::testing::AssertionFailure() << "gimg_stream_create_memory";
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(in);
    return ::testing::AssertionFailure() << "load: " << r;
  }
  GIMG_Raster * raster = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
  if (r != GIMG_OK || !raster) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
    return ::testing::AssertionFailure() << "decode: " << r;
  }
  const GCOL_Color_Info * ci = gimg_raster_color_info_const(raster);
  out.clear();
  if (ci && ci->icc_bytes && ci->icc_size > 0) {
    const uint8_t * p = static_cast<const uint8_t *>(ci->icc_bytes);
    out.assign(p, p + ci->icc_size);
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  return ::testing::AssertionSuccess();
}

} // namespace

TEST(ColorRoundTrip, EveryCodecPairKeepsTheProfile) {
  static const char * const formats[] = {"bmp", "png", "jpeg"};
  const std::vector<uint8_t> profile = a_profile();

  for (const char * from : formats) {
    std::vector<uint8_t> first;
    ASSERT_TRUE(save_as(tagged(profile), from, first)) << "writing " << from;
    {
      SCOPED_TRACE(std::string("written as ") + from);
      std::vector<uint8_t> got;
      ASSERT_TRUE(profile_of(first, got));
      EXPECT_EQ(got, profile) << "a profile must survive being written at all";
    }

    for (const char * to : formats) {
      SCOPED_TRACE(std::string(from) + " -> " + to);
      std::vector<uint8_t> second;
      ASSERT_TRUE(convert(first, to, second));
      std::vector<uint8_t> got;
      ASSERT_TRUE(profile_of(second, got));
      ASSERT_EQ(got.size(), profile.size())
          << "the converted file must carry a profile of the right length";
      EXPECT_EQ(got, profile)
          << "and every byte of it must be the one that went in";
    }
  }
}

TEST(ColorRoundTrip, DropAllCarriesNoProfileAnywhere) {
  // The matrix above is what PRESERVE_ALL promises.  DROP_ALL promises the
  // opposite, and a writer that kept colour under it would be as wrong as one
  // that lost it above.
  static const char * const formats[] = {"bmp", "png", "jpeg"};
  const std::vector<uint8_t> profile = a_profile();

  for (const char * format : formats) {
    SCOPED_TRACE(format);
    GIMG_Raster * raster = tagged(profile);
    ASSERT_NE(raster, nullptr);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
    GIMG_Stream * stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&stream), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.metadata_policy = GIMG_META_DROP_ALL;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, stream, format, &opts, &report), GIMG_OK);
    const void * bytes = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &bytes, &size);
    std::vector<uint8_t> written(static_cast<const uint8_t *>(bytes),
        static_cast<const uint8_t *>(bytes) + size);
    gimg_stream_destroy(stream);
    gimg_doc_destroy(doc);

    std::vector<uint8_t> got;
    ASSERT_TRUE(profile_of(written, got));
    EXPECT_TRUE(got.empty()) << "DROP_ALL must drop the profile too";
  }
}

TEST(ColorRoundTrip, EveryCodecPairKeepsTheResolution) {
  // The resolution takes the same road as the color: it lives on the document
  // and each writer has its own place to put it - biXPelsPerMeter, pHYs, the
  // JFIF density. Each leg is tested in its own codec's tests; this is the
  // matrix, which is where an omission made three times would show.
  static const char * const formats[] = {"bmp", "png", "jpeg"};
  const std::vector<uint8_t> profile = a_profile();

  for (const char * from : formats) {
    GIMG_Raster * raster = tagged(profile);
    ASSERT_NE(raster, nullptr);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
    // A fresh document carries no common metadata until something asks for
    // it, so this is the call that makes the resolution sayable at all.
    GIMG_Meta_Common * meta = nullptr;
    ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &meta), GIMG_OK);
    ASSERT_NE(meta, nullptr);
    gimg_meta_common_set_dpi(meta, 300u, 300u);

    GIMG_Stream * stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&stream), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.metadata_policy = GIMG_META_PRESERVE_ALL;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, stream, from, &opts, &report), GIMG_OK)
        << "writing " << from;
    const void * bytes = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &bytes, &size);
    std::vector<uint8_t> first(static_cast<const uint8_t *>(bytes),
        static_cast<const uint8_t *>(bytes) + size);
    gimg_stream_destroy(stream);
    gimg_doc_destroy(doc);

    for (const char * to : formats) {
      SCOPED_TRACE(std::string(from) + " -> " + to);
      std::vector<uint8_t> second;
      ASSERT_TRUE(convert(first, to, second));

      GIMG_Stream * in = nullptr;
      ASSERT_EQ(gimg_stream_create_memory(second.data(), second.size(), &in),
          GIMG_OK);
      GIMG_Doc * back = nullptr;
      ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &back), GIMG_OK);
      GIMG_Meta_Common * back_meta = gimg_doc_meta_common(back);
      uint32_t x = 0, y = 0;
      if (back_meta) {
        gimg_meta_common_dpi(back_meta, &x, &y);
      }
      EXPECT_EQ(x, 300u) << "horizontal resolution";
      EXPECT_EQ(y, 300u) << "vertical resolution";
      gimg_doc_destroy(back);
      gimg_stream_destroy(in);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
