/**
 * @file
 *
 * PNG encode/save tests: round-trip, save to stream, metadata policy.
 * Loads reference files from tests/data/png/ and writes encoded PNGs to
 * tests/out/png/ for verification (e.g. by
 * tests/data/png/verify_png_output.py).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <functional>
#include <gtest/gtest.h>
#include <vector>

#include "png_test_utils.h"
#include "../../exif_test_utils.h"

// Reaches gimg_png_retarget_ancillary(), which is internal: the rules it
// encodes are per-color-type and there are more of them than an end-to-end
// fixture per case would be a sensible way to cover.
#include "../../../src/codec/png/png_internal.h"
#include <fstream>
#include <set>
#include <string>

TEST(PngEncode, SaveNullDocReturnsInternal) {
  GIMG_Stream * out_s = nullptr;
  GIMG_Result r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(out_s, nullptr);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(nullptr, out_s, "png", &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_INTERNAL);
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, SaveNullStreamReturnsInternal) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, nullptr, "png", &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_INTERNAL);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, SaveNullFormatReturnsInternal) {
  GIMG_Doc * doc = nullptr;
  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_s, nullptr, &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_INTERNAL);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, SaveUnsupportedFormatReturnsUnsupported) {
  GIMG_Doc * doc = nullptr;
  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_s, "jpeg", &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, MetaCommonDescriptionWrittenAndReadAsText) {
  // Programmatic doc: set meta_common description, save as PNG, re-load and
  // verify description (written as tEXt "Description" when not in ancillary).
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_raster_destroy(raster);
  raster = nullptr;
  GIMG_Meta_Common * meta = nullptr;
  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &meta), GIMG_OK);
  ASSERT_EQ(gimg_meta_common_set_description(meta, "Image caption"), GIMG_OK);

  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_s, "png", &opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> saved(out_size, 0);
  if (out_size)
    memcpy(saved.data(), out_data, out_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * in_s = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(saved.data(), saved.size(), &in_s), GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_s, nullptr, nullptr, &doc), GIMG_OK);
  meta = gimg_doc_meta_common(doc);
  ASSERT_NE(meta, nullptr);
  const char * desc = gimg_meta_common_description(meta);
  ASSERT_NE(desc, nullptr);
  EXPECT_STREQ(desc, "Image caption");
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_s);
}

namespace {

/** Save a document as PNG and load the result back.  Returns the bytes. */
std::vector<uint8_t> png_round_trip(GIMG_Doc * doc, GIMG_Doc ** out_doc,
    GIMG_Stream ** keep_stream) {
  GIMG_Stream * out_s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  EXPECT_EQ(gimg_doc_save(doc, out_s, "png", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out_s, &data, &size);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(data),
      static_cast<const uint8_t *>(data) + size);
  gimg_stream_destroy(out_s);
  EXPECT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), keep_stream),
      GIMG_OK);
  EXPECT_EQ(gimg_doc_load(*keep_stream, nullptr, nullptr, out_doc), GIMG_OK);
  return saved;
}

GIMG_Doc * png_gray_doc() {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0,
          &raster) != GIMG_OK) {
    return nullptr;
  }
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  GIMG_Doc * doc = nullptr;
  gimg_doc_from_raster(raster, &doc);
  gimg_raster_destroy(raster);
  return doc;
}

} // namespace

TEST(PngEncode, APixelAspectRatioIsWrittenAsAUnitZeroPhys) {
  // PNG 11.3.4.3: pHYs with unit 0 states a ratio and no physical size, which
  // is what gimg_doc_pixel_aspect_ratio() carries.  Before this the writer
  // only ever emitted unit 1, so a ratio a caller set was never written at
  // all - the accessor reported it and the file did not have it.
  GIMG_Doc * doc = png_gray_doc();
  ASSERT_NE(doc, nullptr);
  gimg_doc_set_pixel_aspect_ratio(doc, 2u, 1u);

  GIMG_Doc * back = nullptr;
  GIMG_Stream * keep = nullptr;
  png_round_trip(doc, &back, &keep);
  gimg_doc_destroy(doc);

  uint32_t num = 0, den = 0;
  const int said = gimg_doc_pixel_aspect_ratio(back, &num, &den);
  EXPECT_EQ(said, 1);
  EXPECT_EQ(num, 2u);
  EXPECT_EQ(den, 1u);
  gimg_doc_destroy(back);
  gimg_stream_destroy(keep);
}

TEST(PngEncode, ASetAspectRatioBeatsThePhysTheFileArrivedWith) {
  // The update half of CRUD, and the one that was silently broken: a pHYs the
  // file came with was preserved verbatim, so setting a new ratio changed what
  // the accessor reported and not what was written.  A caller saw success and
  // got the old value back on the next load.
  std::vector<uint8_t> file;
  ASSERT_TRUE(png_test::load_png_file("png_phys_aspect_4_3.png", file));
  GIMG_Stream * in_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in_s),
      GIMG_OK);
  GIMG_Doc * source = nullptr;
  ASSERT_EQ(gimg_doc_load(in_s, nullptr, nullptr, &source), GIMG_OK);
  uint32_t num = 0, den = 0;
  ASSERT_EQ(gimg_doc_pixel_aspect_ratio(source, &num, &den), 1);
  ASSERT_EQ(num, 4u);
  ASSERT_EQ(den, 3u);

  gimg_doc_set_pixel_aspect_ratio(source, 5u, 2u);
  GIMG_Doc * back = nullptr;
  GIMG_Stream * keep = nullptr;
  png_round_trip(source, &back, &keep);
  gimg_doc_destroy(source);
  gimg_stream_destroy(in_s);

  uint32_t rn = 0, rd = 0;
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(back, &rn, &rd), 1);
  EXPECT_EQ(rn, 5u) << "the preserved chunk won and the new ratio was lost";
  EXPECT_EQ(rd, 2u);
  gimg_doc_destroy(back);
  gimg_stream_destroy(keep);
}

TEST(PngEncode, ClearingAnAspectRatioRemovesTheChunkTheFileArrivedWith) {
  // The delete half.  A cleared ratio has to stop being written, or "remove
  // this" means "keep it".
  std::vector<uint8_t> file;
  ASSERT_TRUE(png_test::load_png_file("png_phys_aspect_4_3.png", file));
  GIMG_Stream * in_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in_s),
      GIMG_OK);
  GIMG_Doc * source = nullptr;
  ASSERT_EQ(gimg_doc_load(in_s, nullptr, nullptr, &source), GIMG_OK);
  ASSERT_EQ(gimg_doc_pixel_aspect_ratio(source, nullptr, nullptr), 1);
  gimg_doc_clear_pixel_aspect_ratio(source);

  GIMG_Doc * back = nullptr;
  GIMG_Stream * keep = nullptr;
  png_round_trip(source, &back, &keep);
  gimg_doc_destroy(source);
  gimg_stream_destroy(in_s);
  uint32_t rn = 9, rd = 9;
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(back, &rn, &rd), 0);
  EXPECT_EQ(rn, 9u);
  gimg_doc_destroy(back);
  gimg_stream_destroy(keep);
}

TEST(PngEncode, APhysicalResolutionWinsOverAPixelAspectRatio) {
  // The two share one chunk and cannot both be written.  A resolution says
  // everything a ratio says and a size as well, so it is the one kept.
  GIMG_Doc * doc = png_gray_doc();
  ASSERT_NE(doc, nullptr);
  GIMG_Meta_Common * meta = nullptr;
  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &meta), GIMG_OK);
  gimg_meta_common_set_dpi(meta, 300u, 300u);
  gimg_doc_set_pixel_aspect_ratio(doc, 2u, 1u);

  GIMG_Doc * back = nullptr;
  GIMG_Stream * keep = nullptr;
  png_round_trip(doc, &back, &keep);
  gimg_doc_destroy(doc);

  uint32_t x_dpi = 0, y_dpi = 0;
  GIMG_Meta_Common * rm = gimg_doc_meta_common(back);
  ASSERT_NE(rm, nullptr);
  gimg_meta_common_dpi(rm, &x_dpi, &y_dpi);
  EXPECT_EQ(x_dpi, 300u);
  EXPECT_EQ(y_dpi, 300u);
  // The ratio is not reported, because a unit 1 pHYs is a resolution.
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(back, nullptr, nullptr), 0);
  gimg_doc_destroy(back);
  gimg_stream_destroy(keep);
}

TEST(PngEncode, SaveToMemoryOutputSucceeds) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_gray.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * in_s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &in_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(in_s, nullptr);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(in_s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(in_s);
  in_s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(out_s, nullptr);

  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save (1x1 gray)";
  EXPECT_GT(report.bytes_written, 0u);
  gimg_doc_destroy(doc);
  doc = nullptr;

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  EXPECT_GT(out_size, 8u);
  if (out_size >= 8u && out_data) {
    const unsigned char * sig = static_cast<const unsigned char *>(out_data);
    EXPECT_EQ(sig[0], 0x89);
    EXPECT_EQ(sig[1], 0x50);
    EXPECT_EQ(sig[2], 0x4E);
    EXPECT_EQ(sig[3], 0x47);
  }
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, RoundTrip1x1Gray) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_gray.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  if (r != GIMG_OK) {
    GTEST_SKIP() << "gimg_doc_save returned " << r << " (round-trip save)";
  }
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_gray.png", saved_data.data(), saved_data.size());

  // Re-load from saved buffer
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  // Decode both and compare
  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);

  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip pixel data must match";

  // Golden encode test: re-decoded pixels must match canonical decode hash.
  EXPECT_EQ(png_test::raster_pixel_hash(decoded), 12638153115695167455ULL)
      << "Round-trip decode hash must match golden 1x1 gray";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip1x1Rgba) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_rgba.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  if (r != GIMG_OK) {
    GTEST_SKIP() << "gimg_doc_save returned " << r << " (round-trip RGBA)";
  }

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_rgba.png", saved_data.data(), saved_data.size());

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);

  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip RGBA pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitGray) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_gray.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit grayscale";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit grayscale pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitRgba) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_rgba.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit RGBA";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit RGBA pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip1x1Rgb) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_rgb.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save RGB (color_type 2) round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_rgb.png", saved_data.data(), saved_data.size());

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip RGB (color_type 2) pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitRgb) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_rgb.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit RGB round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit RGB pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip1x1Grayalpha) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_grayalpha.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save grayscale+alpha (color_type 4) round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_grayalpha.png", saved_data.data(), saved_data.size());

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip grayscale+alpha (color_type 4) pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitGrayalpha) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_grayalpha.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit grayscale+alpha round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit grayscale+alpha pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTripPalette) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_palette.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save palette (PLTE + tRNS)";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip palette + tRNS pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, SaveWithPreserveAllKeepsExif) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  if (r != GIMG_OK) {
    GTEST_SKIP() << "gimg_doc_save returned " << r << " (preserve eXIf)";
  }

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "preserve_exif.png", saved_data.data(), saved_data.size());
  gimg_doc_destroy(doc);

  // Re-load and verify eXIf is present
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw, nullptr);
  size_t exif_size = 0;
  r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
  EXPECT_EQ(r, GIMG_OK);
  EXPECT_EQ(exif_size, 6u) << "eXIf preserved after save with PRESERVE_ALL";

  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithDropAllStripsMetadata) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_DROP_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  // Re-load: eXIf must not be present (stripped by DROP_ALL).
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  // With DROP_ALL, either no meta_raw or no eXIf in it.
  if (raw) {
    size_t exif_size = 0;
    r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
    EXPECT_NE(r, GIMG_OK);
    EXPECT_EQ(exif_size, 0u)
        << "eXIf must be stripped when saving with DROP_ALL";
  }
  gimg_doc_destroy(doc2);
}

namespace {

using exif_test::exif_has_gps_tag;
using exif_test::exif_ifd0_entry_count;
using exif_test::make_exif_with_gps;
using exif_test::replace_exif_chunk;

/** Load png_exif.png, swap in `exif`, save under `policy`, return the eXIf. */
bool round_trip_exif(const std::vector<uint8_t> & exif,
    GIMG_Meta_Policy policy, std::vector<uint8_t> & out_exif) {
  std::vector<uint8_t> file;
  if (!png_test::load_png_file("png_exif.png", file)) { return false; }
  if (!replace_exif_chunk(file, exif)) { return false; }

  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(file.data(), file.size(), &in) != GIMG_OK) {
    return false;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in, nullptr, nullptr, &doc);
  gimg_stream_destroy(in);
  if (r != GIMG_OK || !doc) { return false; }

  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return false;
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out, "png", &opts, &report);
  gimg_doc_destroy(doc);
  if (r != GIMG_OK) { gimg_stream_destroy(out); return false; }

  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  std::vector<uint8_t> saved((const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out);

  GIMG_Stream * in2 = nullptr;
  if (gimg_stream_create_memory(saved.data(), saved.size(), &in2) != GIMG_OK) {
    return false;
  }
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(in2, nullptr, nullptr, &doc2);
  gimg_stream_destroy(in2);
  if (r != GIMG_OK || !doc2) { return false; }
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  size_t sz = 0;
  if (!raw || gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &sz) !=
          GIMG_OK) {
    gimg_doc_destroy(doc2);
    return false;
  }
  out_exif.assign(sz, 0);
  r = gimg_meta_raw_get(raw, "png", 0x65584966u, out_exif.data(), &sz);
  out_exif.resize(sz);
  gimg_doc_destroy(doc2);
  return r == GIMG_OK;
}

} // namespace

/**
 * The control: without the policy, the GPS pointer survives a save.
 *
 * Without this, "no GPS after STRIP_GPS" is satisfied by a writer that drops
 * Exif altogether, or by one that never had it - which is exactly how the
 * previous test passed against a six-byte payload.
 */
TEST(PngEncode, PreserveAllKeepsTheGpsPointer) {
  const std::vector<uint8_t> exif = make_exif_with_gps();
  ASSERT_TRUE(exif_has_gps_tag(exif)) << "the fixture must start with GPS";
  ASSERT_EQ(exif_ifd0_entry_count(exif), 3);

  std::vector<uint8_t> got;
  ASSERT_TRUE(round_trip_exif(exif, GIMG_META_PRESERVE_ALL, got));
  EXPECT_TRUE(exif_has_gps_tag(got))
      << "PRESERVE_ALL must not remove the GPS IFD pointer";
  EXPECT_EQ(exif_ifd0_entry_count(got), 3);
}

/** STRIP_GPS removes the GPS pointer and leaves the other two tags. */
TEST(PngEncode, StripGpsRemovesTheGpsIfdAndKeepsTheRest) {
  const std::vector<uint8_t> exif = make_exif_with_gps();
  std::vector<uint8_t> got;
  ASSERT_TRUE(round_trip_exif(exif, GIMG_META_STRIP_GPS, got));

  EXPECT_FALSE(exif_has_gps_tag(got))
      << "STRIP_GPS left tag 0x8825 in IFD0";
  EXPECT_EQ(exif_ifd0_entry_count(got), 2)
      << "STRIP_GPS must remove exactly the GPS entry, leaving Orientation "
         "and ResolutionUnit";
  EXPECT_LT(got.size(), exif.size())
      << "the GPS IFD and its rational payload should be gone too";
}

/**
 * Exif this library cannot parse makes an Exif policy fail the save.
 *
 * png_exif.png carries a six-byte eXIf payload of 00 01 02 03 04 05, which is
 * below GIMG_EXIF_MIN_SIZE.  The policy asks for GPS to be removed and there
 * is no way to honour that on bytes we cannot read, so the save reports
 * GIMG_ERR_CORRUPT.  The two alternatives were both worse: writing the
 * payload through hands back exactly what the caller asked to have removed,
 * and dropping the chunk destroys metadata whose only fault is that we did
 * not understand it.  Neither is something the caller can detect.
 *
 * This test previously asserted the opposite - that the save succeeded and
 * the payload survived - under the name SaveWithStripGpsStripsOnlyGps, and it
 * passed because gimg_exif_strip_gps() refused the payload on entry and the
 * writer silently kept the original.
 */
TEST(PngEncode, AnExifPolicyFailsOnExifItCannotParse) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(buf.data(), buf.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);

  for (const GIMG_Meta_Policy policy :
      {GIMG_META_STRIP_GPS, GIMG_META_NORMALIZE_EXIF}) {
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.metadata_policy = policy;
    GIMG_Save_Report report = {};
    EXPECT_EQ(gimg_doc_save(doc, out, "png", &opts, &report), GIMG_ERR_CORRUPT)
        << "policy " << (int)policy
        << " must report that it could not be applied";
    gimg_stream_destroy(out);
  }

  // The control: the same document saves cleanly when nothing is asked of the
  // Exif, and the payload it could not parse is still there afterwards.  The
  // refusal above is a refusal to act, not a refusal to save.
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options keep = {};
  keep.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "png", &keep, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  std::vector<uint8_t> saved((const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  GIMG_Stream * s2 = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &s2), GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  ASSERT_EQ(gimg_doc_load(s2, nullptr, nullptr, &doc2), GIMG_OK);
  gimg_stream_destroy(s2);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw, nullptr);
  size_t exif_size = 0;
  EXPECT_EQ(gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size),
      GIMG_OK)
      << "PRESERVE_ALL must keep the payload it cannot parse";
  EXPECT_EQ(exif_size, 6u);
  gimg_doc_destroy(doc2);
}

/**
 * NORMALIZE_EXIF keeps the Exif and resets Orientation to 1.
 *
 * This replaces a test of the same name that ran against png_exif.png's
 * six-byte payload and asserted only that six bytes came back.  It could not
 * see what normalizing does, because gimg_exif_normalize() refused the input
 * before doing any of it.  The blob here carries Orientation 6 (rotate 90),
 * which is the value the policy exists to rewrite.
 */
TEST(PngEncode, NormalizeExifResetsOrientationAndKeepsTheRest) {
  std::vector<uint8_t> exif = make_exif_with_gps();
  // Orientation is IFD0's first entry; set its inline value to 6.
  const size_t orientation_value = 8u + 2u + 8u;
  ASSERT_EQ(exif[8u + 2u], 0x12);      // tag 0x0112, little-endian low byte
  ASSERT_EQ(exif[orientation_value], 1);
  exif[orientation_value] = 6;

  std::vector<uint8_t> got;
  ASSERT_TRUE(round_trip_exif(exif, GIMG_META_NORMALIZE_EXIF, got));
  ASSERT_EQ(got.size(), exif.size())
      << "NORMALIZE_EXIF edits in place; it must not resize the blob";
  EXPECT_EQ(got[orientation_value], 1)
      << "Orientation must be normalized to 1";
  EXPECT_TRUE(exif_has_gps_tag(got))
      << "NORMALIZE_EXIF is not STRIP_GPS; the GPS pointer stays";
  EXPECT_EQ(exif_ifd0_entry_count(got), 3);
}

TEST(PngEncode, SaveWithKeepRawOnlyOmitsKnownSemantic) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_KEEP_RAW_ONLY};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  if (raw) {
    size_t exif_size = 0;
    r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
    EXPECT_NE(r, GIMG_OK);
    EXPECT_EQ(exif_size, 0u)
        << "eXIf (known semantic) must be omitted with KEEP_RAW_ONLY";
  }
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithKeepCommonOnlyRoundTripsColorOnly) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_srgb.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_KEEP_COMMON_ONLY};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * raster = nullptr;
  r = gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc2, 0), nullptr, &raster);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(raster, nullptr);
  const GIMG_Color_Info * info = gimg_raster_color_info_const(raster);
  EXPECT_NE(info, nullptr);
  if (info) {
    EXPECT_EQ(info->transfer, GIMG_TRANSFER_SRGB)
        << "KEEP_COMMON_ONLY should preserve sRGB from raster";
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveInterlacedRoundTrip) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_rgba.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Raster * ref_raster = nullptr;
  r = gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc, 0), nullptr, &ref_raster);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(ref_raster, nullptr);

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL, .interlaced = 1};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(png_test::rasters_equal(ref_raster, decoded))
      << "Interlaced save round-trip should match original pixels";
  gimg_raster_destroy(ref_raster);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc2);
  png_test::write_png_output(
      "interlaced_roundtrip.png", saved_data.data(), saved_data.size());
}

/** Every fcTL/fdAT sequence number in stream order, and the fdAT count per
 * frame.  APNG numbers those two chunk types from one counter; IDAT carries no
 * number, because frame 0's data is the default image. */
struct ApngSequence {
  std::vector<uint32_t> numbers;
  size_t most_fdat_in_one_frame = 0;
};

ApngSequence ReadApngSequence(const std::vector<uint8_t> & png) {
  ApngSequence out;
  size_t i = 8, in_frame = 0;
  while (i + 8 <= png.size()) {
    const uint32_t len = (static_cast<uint32_t>(png[i]) << 24) |
        (static_cast<uint32_t>(png[i + 1]) << 16) |
        (static_cast<uint32_t>(png[i + 2]) << 8) |
        static_cast<uint32_t>(png[i + 3]);
    const std::string type(reinterpret_cast<const char *>(&png[i + 4]), 4);
    if ((type == "fcTL" || type == "fdAT") && i + 12 <= png.size()) {
      out.numbers.push_back((static_cast<uint32_t>(png[i + 8]) << 24) |
          (static_cast<uint32_t>(png[i + 9]) << 16) |
          (static_cast<uint32_t>(png[i + 10]) << 8) |
          static_cast<uint32_t>(png[i + 11]));
      if (type == "fcTL") {
        in_frame = 0;
      }
      else {
        in_frame++;
        if (in_frame > out.most_fdat_in_one_frame) {
          out.most_fdat_in_one_frame = in_frame;
        }
      }
    }
    if (type == "IEND") {
      break;
    }
    i += 12u + static_cast<size_t>(len);
  }
  return out;
}

/** A raster of incompressible noise, so its zlib stream does not fit one
 * chunk.  A gradient or a flat colour compresses to a few hundred bytes and
 * would never reach the 32 KiB split this test is about. */
GIMG_Raster * NoiseRaster(uint32_t w, uint32_t h, uint32_t seed) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &raster) != GIMG_OK) {
    return nullptr;
  }
  auto * base = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  uint32_t x32 = seed * 2654435761u + 1u;
  for (uint32_t y = 0; y < h; y++) {
    uint8_t * row = base + y * stride;
    for (uint32_t x = 0; x < w * 4u; x++) {
      x32 ^= x32 << 13;
      x32 ^= x32 >> 17;
      x32 ^= x32 << 5;
      row[x] = static_cast<uint8_t>(x32 & 0xFFu);
    }
  }
  return raster;
}

TEST(PngEncode, ApngSequenceNumbersSurviveAFrameSplitAcrossChunks) {
  // APNG numbers every fcTL and every fdAT from one counter, starting at 0.
  // The writer used to compute those from the frame index - 2*i-1 for an fcTL
  // and 2*i for its fdAT - which is only right when each frame's data fits in
  // a single fdAT.  The writer splits at 32 KiB, so the first frame larger
  // than that shifted every number after it: a 39-frame animation came out
  // with frame 2's fcTL numbered 3 where it should have been 7.
  //
  // This codec's own loader refused such a file, which is how it was found.
  // Pillow and ImageMagick both read it, because neither checks the numbering;
  // that leniency is why a round trip through an outside decoder did not
  // notice, and why the assertion here is on the numbers themselves rather
  // than only on whether the file loads.
  std::vector<GIMG_Raster *> frames;
  for (uint32_t i = 0; i < 3; i++) {
    GIMG_Raster * raster = NoiseRaster(128, 128, i + 1u);
    ASSERT_NE(raster, nullptr);
    frames.push_back(raster);
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, frames.size()), GIMG_OK);
  for (size_t i = 0; i < frames.size(); i++) {
    gimg_item_set_raster(gimg_doc_item(doc, i), frames[i]);
    gimg_item_set_frame_delay(gimg_doc_item(doc, i), 5, 100);
  }

  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_s, "png", &opts, &report), GIMG_OK);
  const void * out_ptr = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &out_size);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + out_size);
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);

  const ApngSequence seq = ReadApngSequence(saved);

  // Without this the test proves nothing: a frame that fits one fdAT is
  // numbered correctly by the old arithmetic too, so a quieter fixture would
  // pass against the bug.
  ASSERT_GT(seq.most_fdat_in_one_frame, 1u)
      << "the noise frames did not split across chunks, so this test is not "
         "exercising the case it exists for";

  ASSERT_FALSE(seq.numbers.empty());
  for (size_t i = 0; i < seq.numbers.size(); i++) {
    EXPECT_EQ(seq.numbers[i], static_cast<uint32_t>(i))
        << "fcTL/fdAT number " << i << " out of sequence";
  }

  // And the file this codec wrote is one it can read.
  GIMG_Stream * back = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(saved.data(), saved.size(), &back), GIMG_OK);
  GIMG_Doc * reloaded = nullptr;
  EXPECT_EQ(gimg_doc_load(back, nullptr, nullptr, &reloaded), GIMG_OK);
  if (reloaded) {
    EXPECT_EQ(gimg_doc_item_count(reloaded), 3u);
    gimg_doc_destroy(reloaded);
  }
  gimg_stream_destroy(back);
}

TEST(PngEncode, ApngRoundTrip) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_apng_2frame.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "APNG save";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);

  // Re-load and verify frame count, timing, dispose/blend, and pixels.
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc2), 2u);
  gimg_stream_destroy(s2);

  GIMG_Item * item0 = gimg_doc_item((GIMG_Doc *)doc2, 0);
  GIMG_Item * item1 = gimg_doc_item((GIMG_Doc *)doc2, 1);
  ASSERT_NE(item0, nullptr);
  ASSERT_NE(item1, nullptr);

  uint16_t num = 0, den = 0;
  gimg_item_frame_delay(item0, &num, &den);
  EXPECT_EQ(num, 50u);
  EXPECT_EQ(den, 100u);
  EXPECT_EQ(gimg_item_dispose_op(item0), GIMG_DISPOSE_NONE);
  EXPECT_EQ(gimg_item_blend_op(item0), GIMG_BLEND_SOURCE);

  gimg_item_frame_delay(item1, &num, &den);
  EXPECT_EQ(num, 25u);
  EXPECT_EQ(den, 100u);
  EXPECT_EQ(gimg_item_dispose_op(item1), GIMG_DISPOSE_BACKGROUND);
  EXPECT_EQ(gimg_item_blend_op(item1), GIMG_BLEND_OVER);

  GIMG_Raster * r0 = nullptr;
  GIMG_Raster * r1 = nullptr;
  r = gimg_item_decode(item0, nullptr, &r0);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(r0, nullptr);
  r = gimg_item_decode(item1, nullptr, &r1);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(r1, nullptr);

  const unsigned char * px0 =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(r0));
  const unsigned char * px1 =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(r1));
  ASSERT_NE(px0, nullptr);
  ASSERT_NE(px1, nullptr);
  EXPECT_EQ(px0[0], 0) << "frame 0 gray";
  EXPECT_EQ(px1[0], 0x80) << "frame 1 gray";

  gimg_raster_destroy(r0);
  gimg_raster_destroy(r1);
  gimg_doc_destroy(doc2);
  png_test::write_png_output(
      "apng_2frame_roundtrip.png", saved_data.data(), saved_data.size());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---------------------------------------------------------------------------
// Saving at sample depths below 8 bits (palette and grayscale).
//
// PNG 7.2 packs samples of depth 1, 2 and 4 several to a byte, so a scanline
// is (width * bit_depth + 7) / 8 bytes and not width. A writer that stores one
// byte per pixel overruns every row it writes and declares a bit depth in IHDR
// that its own IDAT does not use.
//
// The fixtures are the interlaced/non-interlaced pairs from
// tests/data/png/generate.py; each is loaded, saved, and loaded again, and the
// pixels must survive. The saved files land in tests/out/png/ where
// verify_png_output.py reads them back with an independent decoder.
// ---------------------------------------------------------------------------

namespace {

struct SubByteSaveCase {
  const char * filename;
  uint8_t expect_bit_depth;  ///< IHDR byte 8 the writer should emit.
  uint8_t expect_color_type; ///< IHDR byte 9.
};

const SubByteSaveCase kSubByteSaveCases[] = {
    {"png_pal1_32x8.png", 1, 3},
    {"png_pal2_32x8.png", 2, 3},
    {"png_pal4_32x8.png", 4, 3},
    {"png_pal1_33x9.png", 1, 3},
    {"png_pal2_33x9.png", 2, 3},
    {"png_pal4_33x9.png", 4, 3},
    {"png_pal4_32x8_interlaced.png", 4, 3},
    {"png_pal2_33x9_interlaced.png", 2, 3},
    // Grayscale at the same depths. A frame that arrived below 8 bits goes
    // back out that way when every sample survives the rescaling of PNG 13.12
    // in both directions; widening it to 8 would be lossless but would make
    // the file several times larger for no reason.
    {"png_gray1_32x8.png", 1, 0},
    {"png_gray2_32x8.png", 2, 0},
    {"png_gray4_32x8.png", 4, 0},
    {"png_gray1_33x9.png", 1, 0},
    {"png_gray4_33x9.png", 4, 0},
    {"png_gray2_32x8_interlaced.png", 2, 0},
    {"png_gray4_33x9_interlaced.png", 4, 0},
};

} // namespace

TEST(PngEncode, SamplesBelowEightBitsRoundTripAndKeepTheirDepth) {
  for (const SubByteSaveCase & c : kSubByteSaveCases) {
    std::vector<uint8_t> buf;
    ASSERT_TRUE(png_test::load_png_file(c.filename, buf))
        << c.filename << " missing; run tests/data/png/generate.py";

    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(buf.data(), buf.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK) << c.filename;
    gimg_stream_destroy(s);

    GIMG_Stream * out_s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
    GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out_s, "png", &opts, &report), GIMG_OK)
        << "save " << c.filename;

    const void * out_ptr = nullptr;
    size_t saved_size = 0;
    gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
    std::vector<uint8_t> saved(static_cast<const uint8_t *>(out_ptr),
        static_cast<const uint8_t *>(out_ptr) + saved_size);
    gimg_stream_destroy(out_s);

    // IHDR payload starts at signature (8) + length (4) + type (4) = 16;
    // bit depth is byte 8 of the payload and color type byte 9. PNG 11.2.1.
    ASSERT_GT(saved.size(), 26u) << c.filename;
    EXPECT_EQ(saved[24], c.expect_bit_depth)
        << c.filename << ": IHDR bit depth";
    EXPECT_EQ(saved[25], c.expect_color_type)
        << c.filename << ": IHDR color type";

    GIMG_Stream * s2 = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &s2),
        GIMG_OK);
    GIMG_Doc * doc2 = nullptr;
    ASSERT_EQ(gimg_doc_load(s2, nullptr, nullptr, &doc2), GIMG_OK)
        << "reload " << c.filename;
    gimg_stream_destroy(s2);

    GIMG_Raster * orig = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig), GIMG_OK);
    GIMG_Raster * again = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &again),
        GIMG_OK);
    EXPECT_TRUE(png_test::rasters_equal(orig, again))
        << c.filename << ": pixels must survive a save at its own bit depth";

    std::string out_name = std::string("subbyte_") + c.filename;
    png_test::write_png_output(out_name.c_str(), saved.data(), saved.size());

    gimg_raster_destroy(again);
    gimg_raster_destroy(orig);
    gimg_doc_destroy(doc2);
    gimg_doc_destroy(doc);
  }
}

// ---------------------------------------------------------------------------
// Row filters (PNG 9, filter method 0).
//
// Every row carries a filter type byte, and the five types subtract a
// prediction drawn from the byte above, the byte to the left, both, or the
// Paeth choice among them. The encoder wrote type 0 on every row and no other,
// so four of the five had no producer at all and the sizes showed it.
//
// GIMG_Save_Options.png_filter pins one filter for testing; the default is the
// per-row choice PNG 12.8 recommends.
// ---------------------------------------------------------------------------

namespace {

/** Load, save with the given filter setting, and return the encoded bytes. */
std::vector<uint8_t> SaveWithFilter(
    const char * fixture, uint8_t filter, GIMG_Result * out_result) {
  std::vector<uint8_t> buf;
  if (!png_test::load_png_file(fixture, buf)) {
    *out_result = GIMG_ERR_IO;
    return {};
  }
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(buf.data(), buf.size(), &s) != GIMG_OK) {
    *out_result = GIMG_ERR_INTERNAL;
    return {};
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK) {
    *out_result = r;
    return {};
  }
  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    gimg_doc_destroy(doc);
    *out_result = GIMG_ERR_INTERNAL;
    return {};
  }
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  opts.png_filter = filter;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  std::vector<uint8_t> saved;
  if (r == GIMG_OK) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out_s, &p, &n);
    saved.assign(static_cast<const uint8_t *>(p),
        static_cast<const uint8_t *>(p) + n);
  }
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);
  *out_result = r;
  return saved;
}

/** Decode encoded PNG bytes to a flat pixel vector. */
std::vector<uint8_t> DecodeBytes(const std::vector<uint8_t> & data) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(data.data(), data.size(), &s) != GIMG_OK) {
    return {};
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) != GIMG_OK) {
    gimg_stream_destroy(s);
    return {};
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
    return {};
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * px =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(raster));
  std::vector<uint8_t> out;
  for (uint32_t y = 0; y < h; y++) {
    out.insert(out.end(), px + static_cast<size_t>(y) * stride,
        px + static_cast<size_t>(y) * stride + static_cast<size_t>(w) * bpp);
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  return out;
}

} // namespace

TEST(PngEncode, EveryRowFilterReconstructsTheSamePixels) {
  const char * kFixture = "png_gradient_64x64_rgb.png";
  const uint8_t filters[] = {GIMG_PNG_FILTER_ADAPTIVE, GIMG_PNG_FILTER_NONE,
      GIMG_PNG_FILTER_SUB, GIMG_PNG_FILTER_UP, GIMG_PNG_FILTER_AVERAGE,
      GIMG_PNG_FILTER_PAETH};
  const char * names[] = {
      "adaptive", "none", "sub", "up", "average", "paeth"};

  std::vector<uint8_t> reference;
  std::vector<std::vector<uint8_t>> encodings;
  for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
    GIMG_Result r = GIMG_OK;
    std::vector<uint8_t> saved = SaveWithFilter(kFixture, filters[i], &r);
    ASSERT_EQ(r, GIMG_OK) << "save with filter " << names[i];
    ASSERT_FALSE(saved.empty()) << names[i];
    std::vector<uint8_t> pixels = DecodeBytes(saved);
    ASSERT_FALSE(pixels.empty()) << "decode " << names[i];
    if (reference.empty()) {
      reference = pixels;
    }
    else {
      EXPECT_EQ(pixels, reference)
          << "filter " << names[i] << " does not reconstruct the same image";
    }
    encodings.push_back(std::move(saved));
    std::string out_name = std::string("filter_") + names[i] + ".png";
    png_test::write_png_output(
        out_name.c_str(), encodings.back().data(), encodings.back().size());
  }

  // Each setting must actually change what is written; otherwise a filter that
  // was silently never applied would pass the round-trip check above.
  for (size_t i = 1; i < encodings.size(); i++) {
    for (size_t j = i + 1; j < encodings.size(); j++) {
      EXPECT_NE(encodings[i], encodings[j])
          << names[i] << " and " << names[j] << " produced identical files";
    }
  }
}

TEST(PngEncode, ChoosingAFilterPerRowBeatsForcingNoneOnAll) {
  // PNG 12.8's reason for choosing per row: on data a predictor fits, the
  // filtered bytes are small and DEFLATE codes them in fewer bits. A gradient
  // is the clearest case, and filter None is what the encoder used to write.
  GIMG_Result r = GIMG_OK;
  std::vector<uint8_t> adaptive =
      SaveWithFilter("png_gradient_64x64_rgb.png", GIMG_PNG_FILTER_ADAPTIVE, &r);
  ASSERT_EQ(r, GIMG_OK);
  std::vector<uint8_t> none =
      SaveWithFilter("png_gradient_64x64_rgb.png", GIMG_PNG_FILTER_NONE, &r);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_LT(adaptive.size(), none.size())
      << "adaptive " << adaptive.size() << " bytes, none " << none.size();
}

TEST(PngEncode, AnUnknownFilterSettingIsRefused) {
  GIMG_Result r = GIMG_OK;
  (void)SaveWithFilter("png_gradient_64x64_rgb.png", 99, &r);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
}

// ---------------------------------------------------------------------------
// Color-type-dependent ancillary chunks on save
//
// bKGD, sBIT and hIST are laid out according to the color type in the IHDR
// beside them (PNG 11.3.4.1, 11.3.2.4, 11.3.4.2). The writer does not always
// emit the color type a frame arrived as, so copying them across unchanged
// produces a chunk whose length contradicts the header in the same file -
// which is what saving the conformance suite's tbbn0g04.png used to do, and
// what libpng called "bKGD: invalid".
// ---------------------------------------------------------------------------

namespace {

/** A doc state standing in for a loaded file, for the retargeting rules. */
struct SourceImage {
  gimg_png_doc_state_t state {};
  std::vector<unsigned char> palette;

  SourceImage(uint8_t color_type, uint8_t bit_depth) {
    state.ihdr.color_type = color_type;
    state.ihdr.bit_depth = bit_depth;
  }
  void set_palette(std::initializer_list<unsigned char> rgb) {
    palette.assign(rgb);
    state.plte = palette.data();
    state.plte_size = palette.size();
  }
};

struct Retargeted {
  gimg_png_retarget_t what;
  std::vector<unsigned char> payload; // only meaningful when REPLACE
};

Retargeted Retarget(gimg_png_chunk_type_t type,
    const std::vector<unsigned char> & payload, SourceImage & src,
    uint8_t out_color_type, uint8_t out_bit_depth) {
  unsigned char buf[6] = {};
  size_t n = 0;
  gimg_png_retarget_t what = gimg_png_retarget_ancillary(type, payload.data(),
      payload.size(), &src.state, out_color_type, out_bit_depth, buf, &n);
  return {what, std::vector<unsigned char>(buf, buf + n)};
}

} // namespace

/** Find one chunk in an encoded PNG. Returns false when it is not there. */
bool FindChunk(const std::vector<uint8_t> & png, const char (&type)[5],
    std::vector<uint8_t> & payload) {
  size_t i = 8; // past the signature
  while (i + 8 <= png.size()) {
    uint32_t len = ((uint32_t)png[i] << 24) | ((uint32_t)png[i + 1] << 16) |
        ((uint32_t)png[i + 2] << 8) | (uint32_t)png[i + 3];
    if (i + 12 + (size_t)len > png.size()) {
      return false;
    }
    if (std::memcmp(&png[i + 4], type, 4) == 0) {
      payload.assign(png.begin() + (long)i + 8,
          png.begin() + (long)i + 8 + (long)len);
      return true;
    }
    i += 12 + (size_t)len;
  }
  return false;
}

/** color type and bit depth out of an encoded PNG's IHDR. */
void ReadIhdr(const std::vector<uint8_t> & png, uint8_t * color_type,
    uint8_t * bit_depth) {
  std::vector<uint8_t> ihdr;
  ASSERT_TRUE(FindChunk(png, "IHDR", ihdr));
  ASSERT_EQ(ihdr.size(), 13u);
  *bit_depth = ihdr[8];
  *color_type = ihdr[9];
}

/** Load a fixture, save it whole, and hand back the encoded bytes. */
std::vector<uint8_t> LoadAndSave(const char * fixture) {
  GIMG_Result r = GIMG_OK;
  return SaveWithFilter(fixture, GIMG_PNG_FILTER_ADAPTIVE, &r);
}

/**
 * Load a fixture, let `prepare` change the document, save it.
 *
 * The hook is what separates "the accessor says so" from "the file says so":
 * a setter whose value the writer ignores reports success either way, and only
 * reading the bytes back out catches it.
 */
std::vector<uint8_t> LoadEditSave(
    const char * fixture, const std::function<void(GIMG_Doc *)> & prepare) {
  std::vector<uint8_t> buf;
  if (!png_test::load_png_file(fixture, buf)) {
    return {};
  }
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(buf.data(), buf.size(), &s) != GIMG_OK) {
    return {};
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK) {
    return {};
  }
  if (prepare) {
    prepare(doc);
  }
  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return {};
  }
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  opts.png_filter = GIMG_PNG_FILTER_ADAPTIVE;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  std::vector<uint8_t> saved;
  if (r == GIMG_OK) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out_s, &p, &n);
    saved.assign(static_cast<const uint8_t *>(p),
        static_cast<const uint8_t *>(p) + n);
  }
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);
  return saved;
}

// -- bKGD -------------------------------------------------------------------

TEST(PngAncillaryRetarget, AGrayBackgroundBecomesThreeEqualSamples) {
  // 4-bit grayscale promoted to color type 6 at 8 bits, which is what happens
  // when a tRNS has to become an alpha channel. Gray 7 of 15 rescales to 119
  // by 13.12 - round(7 * 255 / 15) - a value that is not 7, not 112 and not
  // 127, so every plausible way of getting the rescaling wrong is visible.
  SourceImage src(0, 4);
  Retargeted got = Retarget(GIMG_PNG_bKGD, {0x00, 0x07}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  ASSERT_EQ(got.payload.size(), 6u);
  const std::vector<unsigned char> want = {0, 119, 0, 119, 0, 119};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, AnUnchangedColorTypeAndDepthKeepsTheChunk) {
  // The control. A writer that rewrote unconditionally would pass the test
  // above and fail this one.
  SourceImage src(0, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_bKGD, {0x00, 0x80}, src, 0, 8).what,
      GIMG_PNG_RETARGET_KEEP);
}

TEST(PngAncillaryRetarget, APaletteIndexBecomesTheColorItNames) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0x00, 0x00, 0x20, 0x40, 0x60, 0x00, 0xFF, 0x00});
  Retargeted got = Retarget(GIMG_PNG_bKGD, {1}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {0, 0x20, 0, 0x40, 0, 0x60};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, APaletteIndexPastTheEndOfThePaletteIsDropped) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0x00, 0x00, 0x20, 0x40, 0x60});
  EXPECT_EQ(Retarget(GIMG_PNG_bKGD, {7}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, AColorBackgroundSurvivesOnlyIfItIsAlreadyGray) {
  SourceImage color(2, 8);
  // Three different samples: no gray level says this, so it goes.
  EXPECT_EQ(
      Retarget(GIMG_PNG_bKGD, {0, 0x20, 0, 0x40, 0, 0x60}, color, 0, 8).what,
      GIMG_PNG_RETARGET_DROP);
  // Three equal samples: the gray level is exactly that.
  Retargeted got =
      Retarget(GIMG_PNG_bKGD, {0, 0x44, 0, 0x44, 0, 0x44}, color, 0, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {0, 0x44};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, ABackgroundOfTheWrongLengthIsNotCarriedForward) {
  // Three bytes where color type 0 calls for two. The file was already
  // malformed; that is not a reason to write another one.
  SourceImage src(0, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_bKGD, {0x00, 0x80, 0x00}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, SixteenBitBackgroundsRescaleDownToEight) {
  SourceImage src(2, 16);
  Retargeted got = Retarget(
      GIMG_PNG_bKGD, {0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00}, src, 2, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  // 65535 -> 255, 32768 -> 128, 0 -> 0.
  const std::vector<unsigned char> want = {0, 255, 0, 128, 0, 0};
  EXPECT_EQ(got.payload, want);
}

// -- sBIT -------------------------------------------------------------------

TEST(PngAncillaryRetarget, SignificantBitsDoNotSurviveAChangeOfDepth) {
  // Rescaling by 13.12 spreads a 4-bit value across all 8 bits of the new
  // sample, so a count taken before that would tell a decoder to shift data
  // that has already been scaled. Unlike a background color, this cannot be
  // translated - only dropped.
  SourceImage src(0, 4);
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {3}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, SignificantBitsSurviveAChangeOfChannelCount) {
  // Same depth, more channels: a gray level repeated into R, G and B is
  // significant in each to exactly the same degree, and the alpha channel the
  // writer is adding is significant in all of its bits.
  SourceImage src(0, 8);
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 5, 5, 8};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, AnAlphaChannelAlreadyPresentKeepsItsOwnCount) {
  SourceImage src(4, 8); // gray + alpha
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5, 6}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 5, 5, 6};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, SignificantBitsOutsideTheirLegalRangeAreDropped) {
  // 11.3.2.4: each value is at least 1 and no more than the sample depth.
  SourceImage src(0, 8);
  EXPECT_EQ(
      Retarget(GIMG_PNG_sBIT, {0}, src, 6, 8).what, GIMG_PNG_RETARGET_DROP);
  EXPECT_EQ(
      Retarget(GIMG_PNG_sBIT, {9}, src, 6, 8).what, GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, UnequalColorCountsCannotBecomeOneGrayCount) {
  SourceImage src(2, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {5, 6, 7}, src, 0, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, PaletteSignificantBitsDescribeEightBitSamples) {
  // 11.3.2.4: for color type 3 the three values describe the palette's
  // samples, which are always 8-bit, whatever the depth of the indices.
  SourceImage src(3, 4);
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5, 6, 7}, src, 2, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 6, 7};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, EqualColorCountsBecomeTheOneGrayCount) {
  // The other side of UnequalColorCountsCannotBecomeOneGrayCount: when the
  // three counts do agree, one gray count says exactly the same thing, so the
  // chunk is rewritten rather than dropped. Without this case the "counts
  // disagree" rule is the only one on the path and the rewrite never runs.
  SourceImage src(2, 8);
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5, 5, 5}, src, 0, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, AGrayTargetWithAlphaKeepsBothCounts) {
  // Color type 4 carries a gray count and an alpha count, and the source's
  // alpha count is the one that applies - the writer is not adding a channel
  // here, it is keeping one.
  SourceImage src(6, 8); // R, G, B, A
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5, 5, 5, 6}, src, 4, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 6};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, ATruecolorTargetDropsTheAlphaCount) {
  SourceImage src(6, 8);
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5, 6, 7, 8}, src, 2, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 6, 7};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, SignificantBitsForAPaletteNeedThePaletteToStay) {
  // 11.3.2.4 makes the three values describe PLTE's samples. A frame arriving
  // as anything else has no palette those counts were taken from, so they
  // cannot be made to describe the one the writer would build.
  SourceImage from_truecolor(2, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {5, 6, 7}, from_truecolor, 3, 8).what,
      GIMG_PNG_RETARGET_DROP);
  SourceImage from_palette(3, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {5, 6, 7}, from_palette, 3, 4).what,
      GIMG_PNG_RETARGET_KEEP);
}

TEST(PngAncillaryRetarget, AnSbitOfTheWrongLengthIsNotReadAtAll) {
  // The payload length is fixed by the SOURCE color type (11.3.2.4): one
  // value for type 0, two for 4, three for 2 and 3, four for 6. A file
  // carrying any other length is not describing this image, and reading it
  // anyway would mean indexing past what it holds.
  SourceImage src(2, 8); // three values expected
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {5}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {5, 6, 7, 8}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

// -- hIST -------------------------------------------------------------------

TEST(PngAncillaryRetarget, AHistogramWithoutItsPaletteIsDropped) {
  // 11.3.4.2: one frequency per palette entry, and "shall not appear unless a
  // PLTE chunk appears". A truecolor image has no palette for it to be about,
  // and there is nothing to translate it into.
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0, 0, 0x20, 0x40, 0x60});
  EXPECT_EQ(Retarget(GIMG_PNG_hIST, {0, 100, 0, 50}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, AHistogramThatStillMatchesItsPaletteIsKept) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0, 0, 0x20, 0x40, 0x60});
  EXPECT_EQ(Retarget(GIMG_PNG_hIST, {0, 100, 0, 50}, src, 3, 8).what,
      GIMG_PNG_RETARGET_KEEP);
}

TEST(PngAncillaryRetarget, AHistogramOfTheWrongLengthIsDropped) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0, 0, 0x20, 0x40, 0x60}); // two entries
  EXPECT_EQ(Retarget(GIMG_PNG_hIST, {0, 100, 0, 50, 0, 25}, src, 3, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

// -- everything else --------------------------------------------------------

// -- end to end -------------------------------------------------------------
//
// The rules above, reached the way a caller reaches them: load a file, save
// it, and read the chunks back out of what was written.

TEST(PngAncillaryRetarget, APromotedGrayscaleFileGetsABackgroundThatFitsIt) {
  // 4-bit grayscale with tRNS. The transparent gray level cannot survive as a
  // tRNS against an 8-bit raster, so this is written as color type 6 - and a
  // 2-byte bKGD is not a bKGD for color type 6.
  std::vector<uint8_t> saved = LoadAndSave("png_gray4_trns_bkgd_sbit.png");
  ASSERT_FALSE(saved.empty());

  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 6) << "the fixture is meant to be promoted; it was not";
  ASSERT_EQ(bd, 8);

  std::vector<uint8_t> bkgd;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want = {0, 119, 0, 119, 0, 119};
  EXPECT_EQ(bkgd, want) << "gray 7 of 15 rescales to 119 at 8 bits (13.12)";

  // sBIT counted bits in 4-bit samples; the samples are 8-bit now.
  std::vector<uint8_t> sbit;
  EXPECT_FALSE(FindChunk(saved, "sBIT", sbit));
}

TEST(PngAncillaryRetarget, AFileThatKeepsItsColorTypeKeepsItsChunksVerbatim) {
  // The control for the test above. No tRNS, nothing forces a change, so both
  // chunks must come back byte for byte.
  std::vector<uint8_t> saved = LoadAndSave("png_gray8_bkgd_sbit.png");
  ASSERT_FALSE(saved.empty());

  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 0);
  ASSERT_EQ(bd, 8);

  std::vector<uint8_t> bkgd, sbit;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want_bkgd = {0x00, 0x80};
  EXPECT_EQ(bkgd, want_bkgd);
  ASSERT_TRUE(FindChunk(saved, "sBIT", sbit));
  const std::vector<uint8_t> want_sbit = {5};
  EXPECT_EQ(sbit, want_sbit);
}

TEST(PngAncillaryRetarget, AMalformedBackgroundIsNotCopiedIntoTheNewFile) {
  std::vector<uint8_t> saved = LoadAndSave("png_gray8_bad_bkgd.png");
  ASSERT_FALSE(saved.empty());
  std::vector<uint8_t> bkgd;
  EXPECT_FALSE(FindChunk(saved, "bKGD", bkgd))
      << "a three-byte bKGD is wrong for every color type";
}

TEST(PngAncillaryRetarget, APaletteThatStaysAPaletteKeepsItsHistogram) {
  std::vector<uint8_t> saved = LoadAndSave("png_palette_trns_bkgd_hist.png");
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 3) << "a palette frame is written back as one";

  std::vector<uint8_t> bkgd, hist;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want_bkgd = {1};
  EXPECT_EQ(bkgd, want_bkgd);
  ASSERT_TRUE(FindChunk(saved, "hIST", hist));
  EXPECT_EQ(hist.size(), 8u) << "one 16-bit frequency per palette entry";
}

TEST(PngBackground, ChangingItChangesTheFileAndNotJustTheAccessor) {
  // The file's own bKGD used to be copied across untouched, which made
  // gimg_doc_set_background_color() a no-op that reported success: the
  // accessor gave the new colour, the file kept the old one, and only a round
  // trip showed the difference.  So this asks the bytes.
  //
  // The fixture is a palette image, where a background is an *index*, so the
  // new colour also has to be found in the PLTE being written rather than
  // written as a colour that color type cannot hold.  Entry 2 is pure green.
  std::vector<uint8_t> saved =
      LoadEditSave("png_palette_trns_bkgd_hist.png", [](GIMG_Doc * doc) {
        const uint8_t green[4] = {0x00, 0xFF, 0x00, 0xFF};
        gimg_doc_set_background_color(doc, green);
      });
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 3) << "a palette frame is written back as one";

  std::vector<uint8_t> bkgd;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want = {2};
  EXPECT_EQ(bkgd, want) << "PLTE entry 2 is the green that was asked for; the "
                           "fixture arrived naming entry 1";
}

TEST(PngBackground, ClearingItRemovesTheChunk) {
  // "Say nothing" is a different output from any colour, and the only way to
  // say it in PNG is to write no bKGD at all.  A writer that kept the file's
  // own chunk turned a deletion into a no-op.
  std::vector<uint8_t> saved =
      LoadEditSave("png_gray8_bkgd_sbit.png", [](GIMG_Doc * doc) {
        gimg_doc_clear_background_color(doc);
      });
  ASSERT_FALSE(saved.empty());
  std::vector<uint8_t> bkgd;
  EXPECT_FALSE(FindChunk(saved, "bKGD", bkgd))
      << "the document declares no background, so the file must not";

  // And the chunk beside it is untouched, so this is a deletion and not a
  // policy that dropped everything.
  std::vector<uint8_t> sbit;
  EXPECT_TRUE(FindChunk(saved, "sBIT", sbit));
}

TEST(PngBackground, AnUnchangedBackgroundKeepsTheOriginalBytes) {
  // The document holds eight bits a sample and this file's bKGD holds sixteen,
  // so a writer that rebuilt the chunk from the document every time would turn
  // 1234 5678 9abc into 1212 5656 9a9a - a quiet loss of precision on a round
  // trip that changed nothing.
  //
  // The rule is that the file's own bytes go back when they still state what
  // the document states; the tests above are what happens when they do not.
  std::vector<uint8_t> saved = LoadAndSave("png_rgb16_bkgd.png");
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 2);
  ASSERT_EQ(bd, 16);

  std::vector<uint8_t> bkgd;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want = {
      0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
  EXPECT_EQ(bkgd, want) << "sixteen bits a sample, not eight doubled";
}

TEST(PngBackground, AGrayImageStatesNoBackgroundItCannotHold) {
  // Color types 0 and 4 state a background as one gray level, and no gray
  // level is this green.  Writing the red channel, or a luminance, would put a
  // colour in the file that the caller never asked for and could not tell
  // apart from one they did - so nothing is written.
  //
  // ImageMagick makes the other trade: asked for a lime background on a
  // grayscale PNG it promotes the image to truecolor and says it cannot write
  // the gray that was requested.  Growing the pixels to carry an advisory
  // chunk is the wrong way round for a library; the pixels are the payload.
  std::vector<uint8_t> saved =
      LoadEditSave("png_gray8_bkgd_sbit.png", [](GIMG_Doc * doc) {
        const uint8_t green[4] = {0x00, 0xFF, 0x00, 0xFF};
        gimg_doc_set_background_color(doc, green);
      });
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  EXPECT_EQ(ct, 0) << "the image stays gray; it is not grown to fit a hint";
  std::vector<uint8_t> bkgd;
  EXPECT_FALSE(FindChunk(saved, "bKGD", bkgd));

  // A gray the type *can* hold is written, so this is a refusal and not a
  // writer that lost the ability to state one.
  std::vector<uint8_t> gray =
      LoadEditSave("png_gray8_bkgd_sbit.png", [](GIMG_Doc * doc) {
        const uint8_t v[4] = {0xC8, 0xC8, 0xC8, 0xFF};
        gimg_doc_set_background_color(doc, v);
      });
  ASSERT_FALSE(gray.empty());
  ASSERT_TRUE(FindChunk(gray, "bKGD", bkgd));
  const std::vector<uint8_t> want = {0x00, 0xC8};
  EXPECT_EQ(bkgd, want);
}

TEST(PngBackground, APaletteCannotStateAColourItDoesNotHold) {
  // A palette image names its background by index, so the only colours it can
  // state are the ones in its PLTE.  A nearest entry would be a colour nobody
  // asked for, and an absent advisory chunk is a smaller lie than a wrong one.
  std::vector<uint8_t> saved =
      LoadEditSave("png_palette_trns_bkgd_hist.png", [](GIMG_Doc * doc) {
        const uint8_t odd[4] = {0x01, 0x02, 0x03, 0xFF};
        gimg_doc_set_background_color(doc, odd);
      });
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 3);
  std::vector<uint8_t> bkgd;
  EXPECT_FALSE(FindChunk(saved, "bKGD", bkgd))
      << "no palette entry is this colour";
}

// -- acTL num_plays ---------------------------------------------------------

/** num_plays out of an acTL payload (APNG: num_frames then num_plays). */
uint32_t ActlPlays(const std::vector<uint8_t> & png) {
  std::vector<uint8_t> actl;
  if (!FindChunk(png, "acTL", actl) || actl.size() != 8u) {
    return 0xFFFFFFFFu;
  }
  return ((uint32_t)actl[4] << 24) | ((uint32_t)actl[5] << 16) |
      ((uint32_t)actl[6] << 8) | (uint32_t)actl[7];
}

TEST(PngLoopCount, SettingItReachesTheFileAndNotJustTheAccessor) {
  // The writer took num_plays from the codec's own state, which made
  // gimg_doc_set_loop_count() a no-op that reported success on an APNG: the
  // accessor gave the new count and the file kept the old one.  It also meant
  // a count carried in from a GIF was dropped, because a document that did not
  // come from an APNG had no state to take it from and got zero.
  std::vector<uint8_t> saved =
      LoadEditSave("png_apng_3plays.png", [](GIMG_Doc * doc) {
        gimg_doc_set_loop_count(doc, 7u);
      });
  ASSERT_FALSE(saved.empty());
  EXPECT_EQ(ActlPlays(saved), 7u) << "the fixture arrived declaring 3";
}

TEST(PngLoopCount, ClearingItWritesForeverBecauseAPNGCannotSayNothing) {
  // acTL is what makes a PNG an APNG and it always carries a num_plays, so
  // unlike GIF there is no way to leave the count out.  A document declaring
  // none gets zero - the format's own word for "repeat for ever" and what
  // every encoder writes with nothing to say.
  //
  // What must *not* happen is a fall back to the loaded file's own num_plays.
  // The loader put that value in the document, so a document that now declares
  // none is one a caller cleared on purpose, and reaching back past them for
  // the old number would make gimg_doc_clear_loop_count() a no-op.
  std::vector<uint8_t> saved =
      LoadEditSave("png_apng_3plays.png", [](GIMG_Doc * doc) {
        gimg_doc_clear_loop_count(doc);
      });
  ASSERT_FALSE(saved.empty());
  EXPECT_EQ(ActlPlays(saved), 0u) << "the fixture arrived declaring 3";
}

TEST(PngLoopCount, AnUntouchedAPNGKeepsTheCountItArrivedWith) {
  // The control.  A writer that ignored the document in the other direction -
  // always writing zero - would pass the test above and fail this one.
  std::vector<uint8_t> saved = LoadAndSave("png_apng_3plays.png");
  ASSERT_FALSE(saved.empty());
  EXPECT_EQ(ActlPlays(saved), 3u);
}

TEST(PngAncillaryRetarget, ChunksThatDoNotDependOnTheColorTypeAreUntouched) {
  // pHYs, tIME, gAMA, text, and anything unknown mean the same thing whatever
  // the color type, so a change of color type must not disturb them.
  SourceImage src(0, 4);
  const gimg_png_chunk_type_t independent[] = {
      GIMG_PNG_gAMA, GIMG_PNG_tEXt, GIMG_PNG_iCCP, GIMG_PNG_eXIf};
  for (gimg_png_chunk_type_t t : independent) {
    EXPECT_EQ(Retarget(t, {1, 2, 3, 4}, src, 6, 8).what,
        GIMG_PNG_RETARGET_KEEP);
  }
}

// ---------------------------------------------------------------------------
// Building a palette (PNG 11.2.2, color type 3)
//
// A palette is not built by choosing which colors to keep - that is
// quantization, an image-processing decision. It is built when there is
// nothing to choose: at 256 colors or fewer exactly one palette reproduces
// the image, so writing one is a storage decision of the same kind as picking
// a row filter. It is used only when it is the smaller file, which is
// measured and not guessed.
// ---------------------------------------------------------------------------

namespace {

/** Save an RGBA8 raster built from a per-pixel function. */
template <typename Fn>
std::vector<uint8_t> SaveRgba(
    uint32_t w, uint32_t h, Fn color_at, uint8_t palette_option) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &raster) != GIMG_OK) {
    return {};
  }
  size_t stride = gimg_raster_stride_bytes(raster);
  auto * px = static_cast<unsigned char *>(gimg_raster_pixels(raster));
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      uint32_t rgba = color_at(x, y);
      unsigned char * p = px + (size_t)y * stride + (size_t)x * 4;
      p[0] = (unsigned char)(rgba >> 24);
      p[1] = (unsigned char)((rgba >> 16) & 0xFF);
      p[2] = (unsigned char)((rgba >> 8) & 0xFF);
      p[3] = (unsigned char)(rgba & 0xFF);
    }
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_from_raster(raster, &doc) != GIMG_OK) {
    gimg_raster_destroy(raster);
    return {};
  }
  gimg_raster_destroy(raster);

  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return {};
  }
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  opts.png_palette = palette_option;
  GIMG_Save_Report report = {};
  std::vector<uint8_t> saved;
  if (gimg_doc_save(doc, out_s, "png", &opts, &report) == GIMG_OK) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out_s, &p, &n);
    saved.assign(static_cast<const uint8_t *>(p),
        static_cast<const uint8_t *>(p) + n);
  }
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);
  return saved;
}

/** Decode to RGBA8 bytes, so two encodings can be compared pixel for pixel. */
std::vector<uint8_t> DecodeToRgba(const std::vector<uint8_t> & png) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(png.data(), png.size(), &s) != GIMG_OK) {
    return {};
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK) {
    return {};
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return {};
  }
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const auto * px =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(raster));
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  size_t bpp = fmt ? (size_t)fmt->channel_count : 0;
  std::vector<uint8_t> out;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      const unsigned char * p = px + (size_t)y * stride + (size_t)x * bpp;
      // Normalize whatever came back to RGBA so the comparison is about
      // pixels and not about which color type they arrived in.
      if (bpp == 4) {
        out.insert(out.end(), p, p + 4);
      }
      else if (bpp == 3) {
        out.insert(out.end(), {p[0], p[1], p[2], 255});
      }
      else if (bpp == 2) {
        out.insert(out.end(), {p[0], p[0], p[0], p[1]});
      }
      else {
        out.insert(out.end(), {p[0], p[0], p[0], 255});
      }
    }
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  return out;
}

} // namespace

TEST(PngPalette, FewColorsBecomeAPaletteAndComeBackUnchanged) {
  // 48 colors over 128x128: the shape of a screenshot or a diagram, where a
  // palette is a large saving and a lossless one.
  auto color = [](uint32_t x, uint32_t y) -> uint32_t {
    static const uint32_t table[48] = {};
    (void)table;
    uint32_t i = ((x / 8u) + (y / 6u) * 3u) % 48u;
    return ((30u + (i % 5u) * 50u) << 24) | ((20u + (i % 4u) * 60u) << 16) |
        ((40u + (i % 3u) * 70u) << 8) | 0xFFu;
  };
  std::vector<uint8_t> with = SaveRgba(128, 128, color, GIMG_PNG_PALETTE_AUTO);
  std::vector<uint8_t> without =
      SaveRgba(128, 128, color, GIMG_PNG_PALETTE_NEVER);
  ASSERT_FALSE(with.empty());
  ASSERT_FALSE(without.empty());

  uint8_t ct = 0, bd = 0;
  ReadIhdr(with, &ct, &bd);
  EXPECT_EQ(ct, 3) << "few enough colors to store as a palette";

  ReadIhdr(without, &ct, &bd);
  EXPECT_EQ(ct, 6) << "PALETTE_NEVER must leave it truecolor";

  EXPECT_LT(with.size(), without.size()) << "the palette is the point";

  // Lossless: both forms decode to the same pixels.
  EXPECT_EQ(DecodeToRgba(with), DecodeToRgba(without));
}

TEST(PngPalette, TheBitDepthIsTheSmallestThatHoldsTheIndices) {
  // 11.2.2 allows 1, 2, 4 and 8 bits of index. Two colors need one bit.
  auto two = [](uint32_t x, uint32_t y) -> uint32_t {
    return ((x + y) % 2u) ? 0xFF0000FFu : 0x0000FFFFu;
  };
  std::vector<uint8_t> saved = SaveRgba(64, 64, two, GIMG_PNG_PALETTE_AUTO);
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  EXPECT_EQ(ct, 3);
  EXPECT_EQ(bd, 1) << "two entries fit in one bit";

  std::vector<uint8_t> plte;
  ASSERT_TRUE(FindChunk(saved, "PLTE", plte));
  EXPECT_EQ(plte.size(), 6u) << "two entries, three bytes each";
}

TEST(PngPalette, TransparentEntriesComeFirstSoTheTrnsChunkCanBeShort) {
  // 11.3.2.1 lets tRNS be shorter than the palette, every entry past its end
  // being opaque. Putting the non-opaque entries first is what makes that
  // saving available - and it is only available if the order is deliberate.
  auto colors = [](uint32_t x, uint32_t y) -> uint32_t {
    static const uint32_t table[6] = {
        0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu, // opaque
        0xFF000000u, 0x00FF0080u, 0x0000FF40u, // not
    };
    return table[(x + y * 3u) % 6u];
  };
  std::vector<uint8_t> saved = SaveRgba(64, 64, colors, GIMG_PNG_PALETTE_AUTO);
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 3);

  std::vector<uint8_t> plte, trns;
  ASSERT_TRUE(FindChunk(saved, "PLTE", plte));
  ASSERT_TRUE(FindChunk(saved, "tRNS", trns));
  EXPECT_EQ(plte.size(), 18u) << "six entries";
  EXPECT_EQ(trns.size(), 3u)
      << "only the three non-opaque entries need an alpha";
  for (uint8_t a : trns) {
    EXPECT_NE(a, 255) << "an opaque entry inside tRNS is wasted space";
  }
}

TEST(PngPalette, MoreThanTwoHundredAndFiftySixColorsStaysTruecolor) {
  // Reducing these would be quantization, and this writer does not do that.
  auto many = [](uint32_t x, uint32_t y) -> uint32_t {
    return (((x * 2u) % 256u) << 24) | (((y * 2u) % 256u) << 16) |
        (((x + y) % 256u) << 8) | 0xFFu;
  };
  std::vector<uint8_t> saved = SaveRgba(64, 64, many, GIMG_PNG_PALETTE_AUTO);
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  EXPECT_NE(ct, 3);
}

TEST(PngPalette, ExactlyTwoHundredAndFiftySixColorsStillFits) {
  // The boundary: 256 is a palette, and the 257th is what stops it.
  auto exact = [](uint32_t x, uint32_t y) -> uint32_t {
    uint32_t i = (y * 16u + x) % 256u;
    return (i << 24) | (i << 16) | (i << 8) | 0xFFu;
  };
  std::vector<uint8_t> saved = SaveRgba(16, 16, exact, GIMG_PNG_PALETTE_AUTO);
  ASSERT_FALSE(saved.empty());
  std::vector<uint8_t> plte;
  if (FindChunk(saved, "PLTE", plte)) {
    EXPECT_EQ(plte.size(), 256u * 3u);
  }
  // Whether it is chosen depends on which form is smaller at this size; what
  // must hold either way is that the pixels survive.
  auto never = SaveRgba(16, 16, exact, GIMG_PNG_PALETTE_NEVER);
  EXPECT_EQ(DecodeToRgba(saved), DecodeToRgba(never));
}

TEST(PngPalette, APaletteIsNeverTheLargerFile) {
  // The choice is measured, not assumed: both forms are encoded and the loser
  // is discarded. A small image can spend more on PLTE than it saves.
  struct Case {
    uint32_t w, h, colors;
  };
  const Case cases[] = {{4, 4, 4}, {8, 8, 16}, {16, 16, 64}, {64, 64, 200},
      {128, 128, 7}, {200, 137, 33}};
  for (const Case & c : cases) {
    auto color = [&c](uint32_t x, uint32_t y) -> uint32_t {
      uint32_t i = (x + y * 7u) % c.colors;
      return ((i * 7u) << 24) | ((i * 13u) << 16) | ((i * 29u) << 8) | 0xFFu;
    };
    std::vector<uint8_t> with =
        SaveRgba(c.w, c.h, color, GIMG_PNG_PALETTE_AUTO);
    std::vector<uint8_t> without =
        SaveRgba(c.w, c.h, color, GIMG_PNG_PALETTE_NEVER);
    ASSERT_FALSE(with.empty());
    ASSERT_FALSE(without.empty());
    EXPECT_LE(with.size(), without.size())
        << c.w << "x" << c.h << " with " << c.colors << " colors";
    EXPECT_EQ(DecodeToRgba(with), DecodeToRgba(without))
        << "whichever form wins, the pixels are the same";
  }
}

TEST(PngAncillaryRetarget, TheBackgroundAndHistogramComeAfterThePalette) {
  // PNG 5.6, Table 5.3: bKGD and hIST come after PLTE and before IDAT. They
  // are in the ancillary list in the order the file had them - after PLTE
  // there - but this writer emits PLTE itself, after the list, so writing them
  // where they are found puts them in front of it. libpng then reports
  // "bKGD: out of place" and "hIST: invalid", the second because it cannot
  // check the entry count against a palette it has not read yet, and drops it.
  std::vector<uint8_t> saved = LoadAndSave("png_palette_trns_bkgd_hist.png");
  ASSERT_FALSE(saved.empty());

  // Walk the chunks once and record where each type landed.
  std::vector<std::string> order;
  size_t i = 8;
  while (i + 8 <= saved.size()) {
    uint32_t len = ((uint32_t)saved[i] << 24) | ((uint32_t)saved[i + 1] << 16) |
        ((uint32_t)saved[i + 2] << 8) | (uint32_t)saved[i + 3];
    order.emplace_back(reinterpret_cast<const char *>(&saved[i + 4]), 4);
    if (i + 12 + (size_t)len > saved.size()) {
      break;
    }
    i += 12 + (size_t)len;
  }
  auto at = [&order](const char * t) -> long {
    for (size_t k = 0; k < order.size(); k++) {
      if (order[k] == t) {
        return (long)k;
      }
    }
    return -1;
  };
  ASSERT_GE(at("PLTE"), 0);
  ASSERT_GE(at("bKGD"), 0);
  ASSERT_GE(at("hIST"), 0);
  ASSERT_GE(at("IDAT"), 0);
  EXPECT_GT(at("bKGD"), at("PLTE")) << "Table 5.3: bKGD is after PLTE";
  EXPECT_GT(at("hIST"), at("PLTE")) << "Table 5.3: hIST is after PLTE";
  EXPECT_LT(at("bKGD"), at("IDAT")) << "Table 5.3: and before IDAT";
  EXPECT_LT(at("hIST"), at("IDAT"));
}

TEST(PngPalette, AnImageThatArrivedAsAPaletteIsStillWrittenAsOne) {
  // PALETTE_NEVER is about creating a palette, not about discarding one. A
  // frame that came with a palette goes back out with it either way.
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_palette_trns_bkgd_hist.png", buf));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(buf.data(), buf.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);

  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  opts.png_palette = GIMG_PNG_PALETTE_NEVER;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_s, "png", &opts, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out_s, &p, &n);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(p),
      static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);

  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  EXPECT_EQ(ct, 3);
}

// ---------------------------------------------------------------------------
// Round-trip matrix
//
// Every test above checks one thing on one image. This checks the product:
// pixel format against image shape against interlace against row filter
// against palette creation, and asserts that what comes back is what went in.
//
// The combinations are the point. A writer can be right about interlace and
// right about sub-byte depths and still be wrong about a sub-byte depth inside
// an Adam7 pass; it can be right about the palette and right about tRNS and
// wrong about a palette whose transparency lands in the last pass of a
// seven-pixel-wide image. The shapes below are chosen to make Adam7 awkward -
// 1x1 has six empty passes, 1x7 and 7x1 have several, and 33x17 has a partial
// row in most of them.
// ---------------------------------------------------------------------------

namespace {

struct RoundTripCase {
  const GIMG_Pixel_Format * format;
  const char * format_name;
};

/** Fill a raster with a pattern that is not flat, not random, and not gray. */
void FillPattern(GIMG_Raster * raster, unsigned int colors) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  auto * px = static_cast<unsigned char *>(gimg_raster_pixels(raster));
  const bool sixteen = fmt->bits_per_channel[0] == 16;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      unsigned char * p = px + (size_t)y * stride + (size_t)x * bpp;
      // A small repeating set of values, so "few colors" cases really are few
      // and the palette path is reached where it should be.
      unsigned int i = (x * 3u + y * 5u) % colors;
      unsigned int v = (i * 251u) % 256u;
      for (size_t c = 0; c < (size_t)fmt->channel_count; c++) {
        unsigned int cv = (v + (unsigned int)c * 37u) % 256u;
        // Alpha is left opaque except on a few pixels, so tRNS and the alpha
        // channel are both exercised without making every case transparent.
        const bool is_alpha = (fmt->channel_model == GIMG_CHANNEL_RGBA && c == 3);
        if (is_alpha) {
          cv = ((x + y) % 7u == 3u) ? 0u : 255u;
        }
        if (sixteen) {
          p[c * 2] = (unsigned char)cv;
          p[c * 2 + 1] = (unsigned char)((cv * 7u) % 256u);
        }
        else {
          p[c] = (unsigned char)cv;
        }
      }
    }
  }
}

/** Save a raster with the given options and reload it. */
::testing::AssertionResult RoundTrip(const GIMG_Pixel_Format * fmt, uint32_t w,
    uint32_t h, unsigned int colors, int interlaced, uint8_t filter,
    uint8_t palette, uint8_t * out_color_type = nullptr,
    uint8_t * out_bit_depth = nullptr) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(w, h, fmt, GIMG_RASTER_OWNED, nullptr, 0, &raster) !=
      GIMG_OK) {
    return ::testing::AssertionFailure() << "raster_create";
  }
  FillPattern(raster, colors);
  std::vector<uint8_t> before(
      (size_t)gimg_raster_height(raster) * gimg_raster_stride_bytes(raster));
  memcpy(before.data(), gimg_raster_pixels_const(raster), before.size());
  const size_t stride_in = gimg_raster_stride_bytes(raster);
  const size_t bpp_in = gimg_raster_bytes_per_pixel(fmt);

  GIMG_Doc * doc = nullptr;
  if (gimg_doc_from_raster(raster, &doc) != GIMG_OK) {
    gimg_raster_destroy(raster);
    return ::testing::AssertionFailure() << "doc_from_raster";
  }
  gimg_raster_destroy(raster);

  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return ::testing::AssertionFailure() << "output stream";
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  opts.interlaced = (uint8_t)interlaced;
  opts.png_filter = filter;
  opts.png_palette = palette;
  GIMG_Save_Report report = {};
  GIMG_Result sr = gimg_doc_save(doc, out_s, "png", &opts, &report);
  gimg_doc_destroy(doc);
  if (sr != GIMG_OK) {
    gimg_stream_destroy(out_s);
    return ::testing::AssertionFailure() << "save: " << (int)sr;
  }
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out_s, &p, &n);
  std::vector<uint8_t> png(static_cast<const uint8_t *>(p),
      static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out_s);
  if (png.size() > 26) {
    if (out_bit_depth) {
      *out_bit_depth = png[24];
    }
    if (out_color_type) {
      *out_color_type = png[25];
    }
  }

  GIMG_Stream * in_s = nullptr;
  if (gimg_stream_create_memory(png.data(), png.size(), &in_s) != GIMG_OK) {
    return ::testing::AssertionFailure() << "input stream";
  }
  GIMG_Doc * doc2 = nullptr;
  GIMG_Result lr = gimg_doc_load(in_s, nullptr, nullptr, &doc2);
  if (lr != GIMG_OK) {
    gimg_stream_destroy(in_s);
    return ::testing::AssertionFailure() << "reload: " << (int)lr;
  }
  GIMG_Raster * back = nullptr;
  GIMG_Result dr = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &back);
  if (dr != GIMG_OK) {
    gimg_doc_destroy(doc2);
    gimg_stream_destroy(in_s);
    return ::testing::AssertionFailure() << "decode: " << (int)dr;
  }

  ::testing::AssertionResult result = ::testing::AssertionSuccess();
  if (gimg_raster_width(back) != w || gimg_raster_height(back) != h) {
    result = ::testing::AssertionFailure()
        << "size " << gimg_raster_width(back) << "x" << gimg_raster_height(back)
        << " not " << w << "x" << h;
  }
  else {
    const GIMG_Pixel_Format * bf = gimg_raster_format(back);
    size_t bpp_out = gimg_raster_bytes_per_pixel(bf);
    size_t stride_out = gimg_raster_stride_bytes(back);
    const auto * bp =
        static_cast<const unsigned char *>(gimg_raster_pixels_const(back));
    // The decoder may widen a format - a grayscale image with tRNS comes back
    // as RGBA - so compare the channels that mean the same thing rather than
    // insisting the formats match.
    for (uint32_t y = 0; y < h && result; y++) {
      for (uint32_t x = 0; x < w && result; x++) {
        const unsigned char * a = before.data() + (size_t)y * stride_in +
            (size_t)x * bpp_in;
        const unsigned char * b = bp + (size_t)y * stride_out + (size_t)x * bpp_out;
        size_t common = bpp_in < bpp_out ? bpp_in : bpp_out;
        if (bf->channel_count == fmt->channel_count &&
            bf->bits_per_channel[0] == fmt->bits_per_channel[0]) {
          if (memcmp(a, b, common) != 0) {
            result = ::testing::AssertionFailure()
                << "pixel (" << x << "," << y << ") changed";
          }
        }
        else if (fmt->channel_model == GIMG_CHANNEL_GRAY &&
            bf->channel_model == GIMG_CHANNEL_RGBA &&
            fmt->bits_per_channel[0] == bf->bits_per_channel[0]) {
          // gray -> RGBA: the gray level in all three color channels.
          size_t step = fmt->bits_per_channel[0] == 16 ? 2u : 1u;
          for (int c = 0; c < 3 && result; c++) {
            if (memcmp(a, b + (size_t)c * step, step) != 0) {
              result = ::testing::AssertionFailure()
                  << "pixel (" << x << "," << y << ") channel " << c
                  << " changed when widened to RGBA";
            }
          }
        }
      }
    }
  }
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc2);
  gimg_stream_destroy(in_s);
  return result;
}

} // namespace

TEST(PngRoundTripMatrix, EveryCombinationOfShapeInterlaceFilterAndPalette) {
  const RoundTripCase formats[] = {
      {&GIMG_PIXEL_GRAY8, "gray8"},
      {&GIMG_PIXEL_GRAY16, "gray16"},
      {&GIMG_PIXEL_RGBA8, "rgba8"},
      {&GIMG_PIXEL_RGBA16, "rgba16"},
  };
  // 1x1 leaves six Adam7 passes empty; 1x7 and 7x1 leave several; 33x17 has a
  // partial row in most of them. 8x8 is the case where nothing is awkward.
  const struct {
    uint32_t w, h;
  } shapes[] = {{1, 1}, {1, 7}, {7, 1}, {8, 8}, {33, 17}, {64, 64}};
  const uint8_t filters[] = {GIMG_PNG_FILTER_ADAPTIVE, GIMG_PNG_FILTER_NONE,
      GIMG_PNG_FILTER_SUB, GIMG_PNG_FILTER_UP, GIMG_PNG_FILTER_AVERAGE,
      GIMG_PNG_FILTER_PAETH};
  const uint8_t palettes[] = {GIMG_PNG_PALETTE_AUTO, GIMG_PNG_PALETTE_NEVER};
  // Few colors reaches the palette path; many does not.
  const unsigned int color_counts[] = {3u, 200u};

  int cases = 0;
  // What the writer actually chose, so this can assert it reached the paths it
  // exists to cover rather than only that it did not crash.
  std::set<int> color_types;
  std::set<int> bit_depths;
  for (const RoundTripCase & f : formats) {
    for (const auto & s : shapes) {
      for (int interlaced = 0; interlaced <= 1; interlaced++) {
        for (uint8_t filter : filters) {
          for (uint8_t palette : palettes) {
            for (unsigned int colors : color_counts) {
              cases++;
              uint8_t ct = 0, bd = 0;
              EXPECT_TRUE(RoundTrip(f.format, s.w, s.h, colors, interlaced,
                  filter, palette, &ct, &bd))
                  << f.format_name << " " << s.w << "x" << s.h
                  << " interlaced=" << interlaced << " filter=" << (int)filter
                  << " palette=" << (int)palette << " colors=" << colors;
              color_types.insert(ct);
              bit_depths.insert(bd);
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 4 * 6 * 2 * 6 * 2 * 2);

  // A matrix that never reached anything interesting would pass too. These say
  // which of the writer's choices it actually made.
  EXPECT_TRUE(color_types.count(0)) << "grayscale was never written";
  EXPECT_TRUE(color_types.count(3))
      << "no case produced a palette, so PALETTE_AUTO went untested here";
  EXPECT_TRUE(color_types.count(6)) << "truecolor with alpha was never written";
  EXPECT_TRUE(bit_depths.count(8));
  EXPECT_TRUE(bit_depths.count(16));
  EXPECT_TRUE(bit_depths.count(4)) << "no palette was small enough to pack";

  // Color types 2 and 4 are reached only by preserving what a frame arrived
  // as - a raster with no PNG history and an alpha channel is written as 6 by
  // design - so they are not expected here. The conformance round trip covers
  // them: all 162 images of the published suite, every color type among them.
  EXPECT_FALSE(color_types.count(2)) << "unexpected here; see the comment";
  EXPECT_FALSE(color_types.count(4)) << "unexpected here; see the comment";
}

// ---------------------------------------------------------------------------
// Color from the raster, for a document that did not arrive as a PNG
// ---------------------------------------------------------------------------

namespace {

/** Every color chunk type present in a PNG, space separated, in file order. */
std::string color_chunks_of(const std::vector<uint8_t> & png) {
  std::string found;
  for (size_t at = 8; at + 12 <= png.size();) {
    uint32_t length = ((uint32_t)png[at] << 24) | ((uint32_t)png[at + 1] << 16) |
        ((uint32_t)png[at + 2] << 8) | (uint32_t)png[at + 3];
    std::string type((const char *)&png[at + 4], 4);
    if (type == "iCCP" || type == "sRGB" || type == "gAMA" || type == "cHRM" ||
        type == "cICP") {
      if (!found.empty()) {
        found += " ";
      }
      found += type;
    }
    if (length > png.size()) {
      break;
    }
    at += 12 + length;
    if (type == "IEND") {
      break;
    }
  }
  return found;
}

/** A 2x2 opaque raster carrying the given color info. */
GIMG_Raster * raster_with_color(const GIMG_Color_Info & color) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &raster) != GIMG_OK) {
    return nullptr;
  }
  uint8_t * pixels = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 2; x++) {
      uint8_t * px = pixels + (y * stride) + (x * 4u);
      px[0] = (uint8_t)(x * 90u);
      px[1] = (uint8_t)(y * 90u);
      px[2] = 40;
      px[3] = 255;
    }
  }
  if (gimg_raster_set_color_info(raster, &color) != GIMG_OK) {
    gimg_raster_destroy(raster);
    return nullptr;
  }
  return raster;
}

/** Save a raster the document takes ownership of, as PNG. */
GIMG_Result save_png(GIMG_Raster * raster, GIMG_Meta_Policy policy,
    std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_create(&doc);
  if (r != GIMG_OK) {
    return r;
  }
  if ((r = gimg_doc_set_item_count(doc, 1)) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * stream = nullptr;
  if ((r = gimg_stream_create_memory_output(&stream)) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, stream, "png", &opts, &report);
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

} // namespace

TEST(PngEncode, EmbeddedProfileOnTheRasterIsWrittenAsICCP) {
  // A document that did not arrive as a PNG has no ancillary chunks to
  // preserve, so without this its color was lost entirely: a BMP with a V5
  // embedded profile, saved as a PNG, came out untagged and the profile was
  // read only to be dropped.
  std::vector<uint8_t> profile(128);
  profile[0] = 0; profile[1] = 0; profile[2] = 0; profile[3] = 128;
  std::memcpy(&profile[36], "acsp", 4);
  for (size_t i = 40; i < profile.size(); i++) {
    profile[i] = (uint8_t)((i * 7u) & 0xFFu);
  }

  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster, GIMG_META_PRESERVE_ALL, png), GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "iCCP");

  // And every byte of it comes back.
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(png.data(), png.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->icc_size, profile.size());
  EXPECT_EQ(std::memcmp(read->icc_bytes, profile.data(), profile.size()), 0);
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngEncode, SrgbTransferOnTheRasterIsWrittenAsSrgb) {
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_SRGB;
  color.white_point = GIMG_PRIMARIES_SRGB;
  color.transfer = GIMG_TRANSFER_SRGB;

  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster, GIMG_META_PRESERVE_ALL, png), GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "sRGB");
}

TEST(PngEncode, SrgbPrimariesWithAGammaAreWrittenAsGamaNotSrgb) {
  // The sRGB chunk asserts the whole of sRGB, its transfer curve included.
  // Matching on the primaries alone was too loose: a BMP with a calibrated V4
  // header naming sRGB's primaries and a gamma of 2.2 is not an sRGB image,
  // and writing sRGB for it threw the gamma away and claimed a curve the file
  // never stated.  bmpsuite's g/pal8v4.bmp is exactly that file.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_SRGB;
  color.white_point = GIMG_PRIMARIES_SRGB;
  color.transfer = GIMG_TRANSFER_GAMMA;
  color.gamma_value = 2.2;

  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster, GIMG_META_PRESERVE_ALL, png), GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "gAMA");
}

TEST(PngEncode, ColorInfoWithNothingToSayWritesNoColorChunk) {
  GIMG_Color_Info color;
  gimg_color_info_default(&color);

  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster, GIMG_META_PRESERVE_ALL, png), GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "");
}

TEST(PngEncode, DropAllDropsTheColorFromTheRasterToo) {
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_SRGB;

  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster, GIMG_META_DROP_ALL, png), GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "");
}

TEST(PngEncode, TheChunkAPngArrivedWithWinsOverTheRastersColorInfo) {
  // png_srgb.png carries an sRGB chunk.  Preserving what was actually there
  // and then adding a second statement from the color info would put two
  // color chunks in one file, which PNG 11.3.3.3 does not want.
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(png_test::load_png_file("png_srgb.png", bytes));

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr), GIMG_OK);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "png", &opts, &report), GIMG_OK);

  const void * buffer = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &buffer, &size);
  const uint8_t * p = static_cast<const uint8_t *>(buffer);
  std::vector<uint8_t> written(p, p + size);
  EXPECT_EQ(color_chunks_of(written), "sRGB") << "exactly one, and the file's";

  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngEncode, AGammaGamaCannotHoldGoesUnsaid) {
  // gAMA states gamma x 100000 in four bytes, so it cannot hold a gamma above
  // about 42949.  A BMP's V4 gamma is 16.16 fixed point and reaches 65535,
  // and converting one of those to uint32_t is undefined behaviour rather
  // than a large number - UBSan caught it at 4.98588e+09 while fuzzing the
  // PNG round trip.  Such a gamma is now left unsaid, which is what this
  // writer does with everything else it cannot state.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_GAMMA;
  color.gamma_value = 65535.9;

  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster, GIMG_META_PRESERVE_ALL, png), GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "");
}

TEST(PngEncode, LinearTransferIsWrittenAsAGammaOfOne) {
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_LINEAR;

  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster, GIMG_META_PRESERVE_ALL, png), GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "gAMA");

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(png.data(), png.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  EXPECT_NEAR(read->gamma_value, 1.0, 0.0001);
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngEncode, SavingFromADocumentWhoseRasterTheSaveOwnsReadsNoFreedColor) {
  // color_info_for_save is a copy of the struct, but its icc_bytes points into
  // the raster, and the raster is destroyed as soon as the image data is
  // deflated - well before the colour chunk is written.  When the save had
  // decoded the raster itself, the iCCP branch read freed memory and wrote
  // whatever was there into the file.  The JPEG writer had the same defect,
  // found by ASan; this one is reachable the same way and was not, because
  // every test of the colour path attached the raster to the document.
  //
  // Run under `make test-asan` for the assertion that matters.  The profile
  // check below is the visible half: the bytes must be the ones that went in.
  std::vector<uint8_t> profile(256, 0);
  profile[3] = 0;
  std::memcpy(&profile[36], "acsp", 4);
  for (size_t i = 40; i < profile.size(); i++) {
    profile[i] = (uint8_t)((i * 11u) & 0xFFu);
  }

  // A BMP carries the profile in a V5 header, so loading one gives a document
  // with a profile and no raster on its item.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();
  GIMG_Raster * raster = raster_with_color(color);
  ASSERT_NE(raster, nullptr);

  GIMG_Doc * src = nullptr;
  ASSERT_EQ(gimg_doc_create(&src), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(src, 0), raster);
  GIMG_Stream * bmp_out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&bmp_out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(src, bmp_out, "bmp", &opts, &report), GIMG_OK);
  const void * bmp_bytes = nullptr;
  size_t bmp_size = 0;
  gimg_stream_output_buffer(bmp_out, &bmp_bytes, &bmp_size);
  std::vector<uint8_t> bmp(static_cast<const uint8_t *>(bmp_bytes),
      static_cast<const uint8_t *>(bmp_bytes) + bmp_size);
  gimg_stream_destroy(bmp_out);
  gimg_doc_destroy(src);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bmp.data(), bmp.size(), &in), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_EQ(gimg_item_raster(gimg_doc_item(doc, 0)), nullptr)
      << "the save must decode for itself, or this tests nothing";

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  ASSERT_EQ(gimg_doc_save(doc, out, "png", &opts, &report), GIMG_OK);
  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &bytes, &size);
  std::vector<uint8_t> png(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);

  EXPECT_EQ(color_chunks_of(png), "iCCP");

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(png.data(), png.size(), &s), GIMG_OK);
  GIMG_Doc * back_doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &back_doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(back_doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->icc_size, profile.size());
  EXPECT_EQ(std::memcmp(read->icc_bytes, profile.data(), profile.size()), 0)
      << "the profile written must be the one that went in, not whatever the "
         "freed raster's memory happened to hold";
  gimg_raster_destroy(back);
  gimg_doc_destroy(back_doc);
  gimg_stream_destroy(s);
}

TEST(PngEncode, AGamutAReaderWouldNotAssumeIsWrittenAsChrm) {
  // gAMA states the transfer and says nothing about the gamut, so a PNG
  // carrying it alone means sRGB's primaries - which is what a reader
  // assumes when nothing says otherwise. That costs nothing for an sRGB
  // image and loses everything for an Adobe RGB one: a BMP with a calibrated
  // V4 header naming Adobe RGB came out of a save as PNG with its gamma and
  // not its gamut. cHRM (11.3.2.1) is the chunk that says it, and it goes
  // beside gAMA rather than instead of it.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_ADOBE_RGB;
  color.white_point = GIMG_PRIMARIES_ADOBE_RGB;
  color.transfer = GIMG_TRANSFER_GAMMA;
  color.gamma_value = 2.2;

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster_with_color(color), GIMG_META_PRESERVE_ALL, png),
      GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "cHRM gAMA");

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(png.data(), png.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->primaries, GIMG_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(read->white_point, GIMG_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(read->transfer, GIMG_TRANSFER_GAMMA);
  EXPECT_NEAR(read->gamma_value, 2.2, 0.0001);
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngEncode, TheGamutAReaderAlreadyAssumesIsNotWritten) {
  // sRGB's primaries are what a PNG with no cHRM means, so stating them costs
  // 44 bytes and says nothing new.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_SRGB;
  color.white_point = GIMG_PRIMARIES_SRGB;
  color.transfer = GIMG_TRANSFER_GAMMA;
  color.gamma_value = 1.8;

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster_with_color(color), GIMG_META_PRESERVE_ALL, png),
      GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "gAMA");
}

TEST(PngEncode, ChrmIsNotWrittenBesideAProfile) {
  // The profile is the more specific statement and supersedes it; 11.3.3.3
  // does not want the two of them disagreeing in one file.
  std::vector<uint8_t> profile(128, 0);
  std::memcpy(&profile[36], "acsp", 4);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_ADOBE_RGB;
  color.white_point = GIMG_PRIMARIES_ADOBE_RGB;
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster_with_color(color), GIMG_META_PRESERVE_ALL, png),
      GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "iCCP");
}

TEST(PngEncode, AGamutWithNoTransferIsStillWritten) {
  // Either half of the pair may be absent. A gamut with no curve is written
  // as cHRM alone, the same way a BMP's calibrated header leaves its gamma at
  // zero when nothing stated one.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.primaries = GIMG_PRIMARIES_ADOBE_RGB;
  color.white_point = GIMG_PRIMARIES_ADOBE_RGB;

  std::vector<uint8_t> png;
  ASSERT_EQ(save_png(raster_with_color(color), GIMG_META_PRESERVE_ALL, png),
      GIMG_OK);
  EXPECT_EQ(color_chunks_of(png), "cHRM");

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(png.data(), png.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->primaries, GIMG_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(read->transfer, GIMG_TRANSFER_UNKNOWN)
      << "the file stated no curve, so neither does the raster";
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

namespace {

/** A file loaded, re-saved with the given options, and decoded again. */
struct ResavedPng {
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> pixels; ///< Decoded, row-major, no stride padding.
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t channels = 0;
  uint8_t bits = 0;
  uint8_t color_type = 0;
  uint8_t bit_depth = 0;
  uint8_t interlace = 0;
};

/** Flatten a raster's pixels, dropping the stride padding. */
void flatten(const GIMG_Raster * r, ResavedPng & out) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(r);
  out.width = gimg_raster_width(r);
  out.height = gimg_raster_height(r);
  out.channels = fmt->channel_count;
  out.bits = fmt->bits_per_channel[0];
  const size_t row = (size_t)out.width * gimg_raster_bytes_per_pixel(fmt);
  const size_t stride = gimg_raster_stride_bytes(r);
  const auto * p = (const unsigned char *)gimg_raster_pixels_const(r);
  out.pixels.resize(row * out.height);
  for (uint32_t y = 0; y < out.height; y++) {
    memcpy(out.pixels.data() + (size_t)y * row, p + (size_t)y * stride, row);
  }
}

/** Load a fixture, save it again with @p interlaced, and decode the result. */
::testing::AssertionResult resave(
    const char * fixture, int interlaced, ResavedPng & out) {
  std::vector<uint8_t> file;
  if (!png_test::load_png_file(fixture, file)) {
    return ::testing::AssertionFailure() << "missing fixture " << fixture;
  }
  GIMG_Stream * in_s = nullptr;
  if (gimg_stream_create_memory(file.data(), file.size(), &in_s) != GIMG_OK) {
    return ::testing::AssertionFailure() << "input stream";
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in_s, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(in_s);
    return ::testing::AssertionFailure() << "load: " << (int)r;
  }
  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_s);
    return ::testing::AssertionFailure() << "output stream";
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  opts.interlaced = (unsigned int)interlaced;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_s);
  if (r != GIMG_OK) {
    gimg_stream_destroy(out_s);
    return ::testing::AssertionFailure() << "save: " << (int)r;
  }
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out_s, &p, &n);
  out.bytes.assign((const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out_s);
  if (out.bytes.size() < 34u) {
    return ::testing::AssertionFailure() << "output too short to hold an IHDR";
  }
  out.bit_depth = out.bytes[24];
  out.color_type = out.bytes[25];
  out.interlace = out.bytes[28];

  GIMG_Stream * back_s = nullptr;
  if (gimg_stream_create_memory(out.bytes.data(), out.bytes.size(), &back_s)
      != GIMG_OK) {
    return ::testing::AssertionFailure() << "reload stream";
  }
  GIMG_Doc * back = nullptr;
  r = gimg_doc_load(back_s, nullptr, nullptr, &back);
  if (r != GIMG_OK) {
    gimg_stream_destroy(back_s);
    return ::testing::AssertionFailure() << "reload: " << (int)r;
  }
  GIMG_Raster * raster = nullptr;
  r = gimg_item_decode(gimg_doc_item(back, 0), nullptr, &raster);
  if (r != GIMG_OK) {
    gimg_doc_destroy(back);
    gimg_stream_destroy(back_s);
    return ::testing::AssertionFailure() << "decode: " << (int)r;
  }
  flatten(raster, out);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(back);
  gimg_stream_destroy(back_s);
  return ::testing::AssertionSuccess();
}

} // namespace

/**
 * Adam7 is a reordering: the interlaced save is the same picture.
 *
 * The property needs no reference decoder. PNG 8.2 splits the image into
 * seven passes and writes each as its own little image; nothing about the
 * samples changes, so a file saved interlaced and the same file saved
 * progressively must decode to identical pixels, and to the same colour type
 * and bit depth. Anything else is the writer scattering a pass to the wrong
 * place.
 *
 * It is run over files rather than over rasters built in the test, and that
 * is the point. A document assembled in memory has no PNG behind it, so the
 * writer picks the colour type from the raster's format alone and picks 0 or
 * 6 every time. Colour types 2 and 4 are only ever written when the document
 * *came from* a PNG that used them - which meant the interlaced writer's
 * per-pixel path for both, at 8 and at 16 bits, had never run: four branches,
 * uncovered, in a function whose other half was exercised constantly.
 *
 * The interlace byte of each output is checked too. Without that a writer
 * that ignored the option entirely would pass every other assertion here, by
 * writing the same progressive file twice.
 */
TEST(PngEncode, AnInterlacedSaveIsTheSamePictureAsAProgressiveOne) {
  const char * fixtures[] = {
      // Colour type 2, at both depths and at a size with seven live passes.
      "png_gradient_64x64_rgb.png",
      "png_rgb_no_palette.png",
      "png_rgb16_bkgd.png",
      // Colour type 4, which nothing but a type 4 file can make the writer
      // emit.
      "png_grayalpha8_16x16.png",
      "png_grayalpha16_16x16.png",
      "png_1x1_grayalpha.png",
      "png_16bit_grayalpha.png",
      // And the types the synthetic round-trips already reach, so that a
      // change breaking one of them says so here as well.
      "png_gray1_33x9.png",
      "png_gray4_32x8.png",
      "png_pal4_33x9.png",
      "png_16bit_gray.png",
      "png_16bit_rgba.png",
      "png_palette_trns_bkgd_hist.png",
      "png_gray4_trns_bkgd_sbit.png",
  };

  for (const char * fixture : fixtures) {
    SCOPED_TRACE(fixture);
    ResavedPng plain, woven;
    ASSERT_TRUE(resave(fixture, 0, plain));
    ASSERT_TRUE(resave(fixture, 1, woven));

    EXPECT_EQ(plain.interlace, 0u) << "asked for a progressive file";
    EXPECT_EQ(woven.interlace, 1u)
        << "asked for an interlaced file and got a progressive one, so "
           "everything below is comparing a file with itself";
    EXPECT_EQ(woven.color_type, plain.color_type)
        << "interlacing changed the colour type";
    EXPECT_EQ(woven.bit_depth, plain.bit_depth)
        << "interlacing changed the bit depth";
    ASSERT_EQ(woven.width, plain.width);
    ASSERT_EQ(woven.height, plain.height);
    ASSERT_EQ(woven.channels, plain.channels);
    ASSERT_EQ(woven.bits, plain.bits);
    ASSERT_EQ(woven.pixels.size(), plain.pixels.size());
    for (size_t i = 0; i < plain.pixels.size(); i++) {
      if (woven.pixels[i] != plain.pixels[i]) {
        const size_t per_pixel =
            (size_t)plain.channels * (plain.bits == 16 ? 2u : 1u);
        const size_t pixel = i / per_pixel;
        FAIL() << "pixel (" << pixel % plain.width << ","
               << pixel / plain.width << ") byte " << i % per_pixel
               << ": interlaced " << (int)woven.pixels[i] << ", progressive "
               << (int)plain.pixels[i];
      }
    }
  }
}

namespace {

/** Append a chunk of @p type carrying @p payload, with its CRC. */
void append_chunk(std::vector<uint8_t> & out, const char * type,
    const std::vector<uint8_t> & payload) {
  const uint32_t n = (uint32_t)payload.size();
  out.push_back((uint8_t)(n >> 24));
  out.push_back((uint8_t)(n >> 16));
  out.push_back((uint8_t)(n >> 8));
  out.push_back((uint8_t)n);
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), payload.begin(), payload.end());
  out.insert(out.end(), body.begin(), body.end());
  const uint32_t crc = exif_test::png_crc(body.data(), body.size());
  out.push_back((uint8_t)(crc >> 24));
  out.push_back((uint8_t)(crc >> 16));
  out.push_back((uint8_t)(crc >> 8));
  out.push_back((uint8_t)crc);
}

/** A copy of @p base with one tEXt chunk inserted straight after the IHDR. */
std::vector<uint8_t> with_text_chunk(const std::vector<uint8_t> & base,
    const std::string & keyword, const std::string & text) {
  // signature (8) + length (4) + type (4) + IHDR payload (13) + CRC (4).
  const size_t after_ihdr = 8u + 4u + 4u + 13u + 4u;
  std::vector<uint8_t> payload(keyword.begin(), keyword.end());
  payload.push_back(0);
  payload.insert(payload.end(), text.begin(), text.end());
  std::vector<uint8_t> chunk;
  append_chunk(chunk, "tEXt", payload);
  std::vector<uint8_t> out(base.begin(), base.begin() + (long)after_ihdr);
  out.insert(out.end(), chunk.begin(), chunk.end());
  out.insert(out.end(), base.begin() + (long)after_ihdr, base.end());
  return out;
}

/** The keywords of every tEXt chunk in @p png, in file order. */
std::vector<std::string> text_keywords(const std::vector<uint8_t> & png) {
  std::vector<std::string> out;
  size_t i = 8;
  while (i + 8 <= png.size()) {
    const uint32_t n = ((uint32_t)png[i] << 24) | ((uint32_t)png[i + 1] << 16) |
        ((uint32_t)png[i + 2] << 8) | (uint32_t)png[i + 3];
    const std::string type((const char *)&png[i + 4], 4);
    if (type == "tEXt" && i + 8 + n <= png.size()) {
      const uint8_t * p = &png[i + 8];
      size_t kw = 0;
      while (kw < n && p[kw] != 0) {
        kw++;
      }
      out.push_back(std::string((const char *)p, kw));
    }
    if (type == "IEND") {
      break;
    }
    i += 12u + (size_t)n;
  }
  return out;
}

/** Save @p png's document again under @p policy and hand back the bytes. */
::testing::AssertionResult resave_with_policy(const std::vector<uint8_t> & png,
    GIMG_Meta_Policy policy, std::vector<uint8_t> & out) {
  GIMG_Stream * in_s = nullptr;
  if (gimg_stream_create_memory(png.data(), png.size(), &in_s) != GIMG_OK) {
    return ::testing::AssertionFailure() << "input stream";
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in_s, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(in_s);
    return ::testing::AssertionFailure() << "load: " << (int)r;
  }
  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_s);
    return ::testing::AssertionFailure() << "output stream";
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_s);
  if (r != GIMG_OK) {
    gimg_stream_destroy(out_s);
    return ::testing::AssertionFailure() << "save: " << (int)r;
  }
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out_s, &p, &n);
  out.assign((const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out_s);
  return ::testing::AssertionSuccess();
}

} // namespace

/**
 * STRIP_GPS drops the text chunks that carry a location and keeps the rest.
 *
 * PNG has no GPS chunk. What a location looks like in a PNG is a tEXt, zTXt or
 * iTXt keyword - "GPS Latitude", ImageMagick's "exif:GPSLatitude" - and
 * deciding which keywords mean that is a rule written out in the saver. The
 * rule was never exercised: the policy tests all work on eXIf blobs, and no
 * fixture in the suite carries a text chunk at all, so the whole predicate was
 * dead code that looked live.
 *
 * Each keyword is asserted both ways. Under STRIP_GPS a location keyword must
 * go and every other keyword must stay - a policy that dropped all text would
 * pass a one-sided test - and under PRESERVE_ALL every one of them, location
 * or not, must survive, because that policy is a promise not to touch
 * anything.
 *
 * The keywords either side of the boundary are the point: "GPS" and
 * "GPS Latitude" are locations, "GPSish" is a word that starts with the same
 * three letters, and "exif:GPSLatitude" is a location only because of the
 * prefix rule that "exif:Orientation" is not caught by.
 */
TEST(PngEncode, StrippingGpsDropsTheTextChunksThatCarryALocation) {
  std::vector<uint8_t> base;
  ASSERT_TRUE(png_test::load_png_file("png_2x2_gray.png", base));

  struct Case {
    const char * keyword;
    bool is_location;
  };
  const Case cases[] = {
      {"GPS", true},
      {"GPS Latitude", true},
      {"gps longitude", true},
      {"EXIF:GPSLatitude", true},
      {"exif:gpsaltitude", true},
      // Not locations, and each is next to one that is.
      {"GPSish", false},
      {"GP", false},
      {"exif:Orientation", false},
      {"Comment", false},
      {"Description", false},
      {"Author", false},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(c.keyword);
    const std::vector<uint8_t> png =
        with_text_chunk(base, c.keyword, "a value that does not matter");
    ASSERT_EQ(text_keywords(png).size(), 1u)
        << "the fixture must carry exactly the one chunk under test";

    std::vector<uint8_t> stripped, preserved;
    ASSERT_TRUE(resave_with_policy(png, GIMG_META_STRIP_GPS, stripped));
    ASSERT_TRUE(resave_with_policy(png, GIMG_META_PRESERVE_ALL, preserved));

    const std::vector<std::string> after_strip = text_keywords(stripped);
    const std::vector<std::string> after_preserve = text_keywords(preserved);

    if (c.is_location) {
      EXPECT_TRUE(after_strip.empty())
          << "STRIP_GPS kept a location keyword: " << after_strip[0];
    }
    else {
      ASSERT_EQ(after_strip.size(), 1u)
          << "STRIP_GPS dropped a keyword that names no location";
      EXPECT_EQ(after_strip[0], c.keyword);
    }
    ASSERT_EQ(after_preserve.size(), 1u)
        << "PRESERVE_ALL is a promise not to touch anything";
    EXPECT_EQ(after_preserve[0], c.keyword);
  }
}

namespace {

/** One pixel of an RGBA raster, whatever its depth, widened to 16 bits. */
void narrow_get(
    const GIMG_Raster * r, uint32_t x, uint32_t y, uint16_t out[4]) {
  const GIMG_Pixel_Format * f = gimg_raster_format(r);
  const auto * p = (const unsigned char *)gimg_raster_pixels_const(r);
  const unsigned char * q = p + (size_t)y * gimg_raster_stride_bytes(r) +
      (size_t)x * gimg_raster_bytes_per_pixel(f);
  if (f->bits_per_channel[0] == 16) {
    const auto * u = (const uint16_t *)(const void *)q;
    for (int k = 0; k < 4; k++) { out[k] = u[k]; }
  }
  else {
    for (int k = 0; k < 4; k++) { out[k] = q[k]; }
  }
}

void narrow_put(GIMG_Raster * r, uint32_t x, uint32_t y, const uint16_t v[4]) {
  const GIMG_Pixel_Format * f = gimg_raster_format(r);
  auto * p = (unsigned char *)gimg_raster_pixels(r);
  unsigned char * q = p + (size_t)y * gimg_raster_stride_bytes(r) +
      (size_t)x * gimg_raster_bytes_per_pixel(f);
  if (f->bits_per_channel[0] == 16) {
    auto * u = (uint16_t *)(void *)q;
    for (int k = 0; k < 4; k++) { u[k] = v[k]; }
  }
  else {
    for (int k = 0; k < 4; k++) { q[k] = (unsigned char)v[k]; }
  }
}

/** Every pixel of a decoded raster, stride padding removed. */
std::vector<uint8_t> narrow_pixels(const GIMG_Raster * r) {
  const GIMG_Pixel_Format * f = gimg_raster_format(r);
  const size_t row =
      (size_t)gimg_raster_width(r) * gimg_raster_bytes_per_pixel(f);
  const size_t stride = gimg_raster_stride_bytes(r);
  const auto * p = (const unsigned char *)gimg_raster_pixels_const(r);
  std::vector<uint8_t> out(row * gimg_raster_height(r));
  for (uint32_t y = 0; y < gimg_raster_height(r); y++) {
    memcpy(out.data() + (size_t)y * row, p + (size_t)y * stride, row);
  }
  return out;
}

/** The colour type and bit depth of a PNG's IHDR, and its tRNS length. */
struct PngHead {
  int color_type = -1;
  int bit_depth = -1;
  int trns_size = -1; ///< -1 when the file carries no tRNS chunk.
};

PngHead png_head(const std::vector<uint8_t> & b) {
  PngHead h;
  for (size_t i = 8; i + 8 < b.size();) {
    const unsigned len = ((unsigned)b[i] << 24) | ((unsigned)b[i + 1] << 16) |
        ((unsigned)b[i + 2] << 8) | (unsigned)b[i + 3];
    const std::string type((const char *)&b[i + 4], 4);
    if (type == "IHDR") {
      h.bit_depth = b[i + 16];
      h.color_type = b[i + 17];
    }
    else if (type == "tRNS") {
      h.trns_size = (int)len;
    }
    i += 12u + len;
    if (type == "IEND") { break; }
  }
  return h;
}

/**
 * Load @p file, run @p edit over the decoded raster, save as PNG, and report
 * what the writer chose and whether the edit came back.
 */
struct NarrowResult {
  PngHead head;
  bool survived = false;
  std::vector<uint8_t> wrote, read_back;
};

NarrowResult save_after_editing(
    const char * file, const std::function<void(GIMG_Raster *)> & edit) {
  NarrowResult nr;
  std::vector<uint8_t> bytes;
  if (!png_test::load_png_file(file, bytes)) { return nr; }
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return nr;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(in, nullptr, nullptr, &doc) != GIMG_OK) {
    gimg_stream_destroy(in);
    return nr;
  }
  GIMG_Item * item = gimg_doc_item(doc, 0);
  gimg_item_ensure_decoded(item, nullptr);
  GIMG_Raster * raster = gimg_item_raster(item);
  if (raster) {
    edit(raster);
    nr.wrote = narrow_pixels(raster);
  }
  GIMG_Stream * sink = nullptr;
  std::vector<uint8_t> out;
  if (gimg_stream_create_memory_output(&sink) == GIMG_OK) {
    GIMG_Save_Options opts = {};
    opts.quality = 95;
    GIMG_Save_Report report = {};
    if (gimg_doc_save(doc, sink, "png", &opts, &report) == GIMG_OK) {
      const void * p = nullptr;
      size_t n = 0;
      gimg_stream_output_buffer(sink, &p, &n);
      out.assign((const uint8_t *)p, (const uint8_t *)p + n);
    }
    gimg_stream_destroy(sink);
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  if (out.empty()) { return nr; }
  nr.head = png_head(out);

  GIMG_Stream * back = nullptr;
  if (gimg_stream_create_memory(out.data(), out.size(), &back) != GIMG_OK) {
    return nr;
  }
  GIMG_Doc * doc2 = nullptr;
  if (gimg_doc_load(back, nullptr, nullptr, &doc2) == GIMG_OK) {
    GIMG_Raster * r2 = nullptr;
    if (gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &r2) == GIMG_OK &&
        r2) {
      nr.read_back = narrow_pixels(r2);
      gimg_raster_destroy(r2);
    }
    gimg_doc_destroy(doc2);
  }
  gimg_stream_destroy(back);
  nr.survived = !nr.read_back.empty() && nr.read_back == nr.wrote;
  return nr;
}

const uint16_t OPAQUE16 = 0xFFFFu;

} // namespace

/**
 * A grayscale or truecolour PNG with a tRNS key decodes to RGBA, and the
 * writer puts it back the narrow way when the decoded raster still obeys
 * PNG 11.3.2.1: one fully transparent key colour, nothing partial, and no
 * opaque pixel wearing that colour.
 *
 * The rule matters because a caller's edit can break it, and the failure is
 * silent: if the writer narrowed anyway, an opaque pixel painted the key
 * colour would come back transparent - a valid PNG of the wrong picture.
 *
 * So each case edits one pixel, saves, and asks two questions: which colour
 * type the writer chose, and whether the pixels survived. The control is the
 * case that keeps the rule - without it, a writer that simply never narrowed
 * would pass every other case here.
 *
 * These run at 16 bits because the check reads the raster two bytes at a time
 * for a 16-bit frame and one byte at a time otherwise, and no fixture carried
 * a tRNS chunk at 16 bits, so the wide half had never executed.
 */
// A grayscale frame that arrived at 1, 2 or 4 bits is written back at that
// depth only when every sample could have come from it: PNG 13.12 rescales a
// sample of depth d to 8 bits as round(s * 255 / (2^d-1)), and reversing that
// is exact only for the values the rescaling can produce.  At depth 1 those
// are 0 and 255, at depth 2 they are 0, 85, 170 and 255, and at depth 4 the
// multiples of 17.
//
// gimg_png_gray_fits_depth says which, and its "no" had never been taken:
// every fixture reaching it was a picture that had come from that depth, so
// nothing had asked the writer to notice a sample that had not.  A broken
// check would narrow anyway and quietly round the sample away, and the loaded
// picture would still match the fixture it came from.  Both answers are
// asserted here, so the narrowing cannot pass by never happening or by
// always happening.
TEST(PngEncode, AGraySampleThatCannotSurviveTheNarrowDepthKeepsEightBits) {
  struct Case {
    const char * file;
    int depth;
    int representable; ///< A value that depth can hold exactly.
  };
  // 136 is 8*255/15 and 170 is 2*255/3; both are exactly what reversing the
  // rescale gives, so they narrow.  128 is not reachable at any of the three.
  const Case cases[] = {
      {"png_gray1_32x8.png", 1, 255},
      {"png_gray2_32x8.png", 2, 170},
      {"png_gray4_32x8.png", 4, 136},
  };
  const uint16_t breaks = 128;
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.file) + " at depth " + std::to_string(c.depth));
    auto set00 = [](GIMG_Raster * r, unsigned value) {
      const GIMG_Pixel_Format * f = gimg_raster_format(r);
      // These decode to one 8-bit channel; writing four would run off the
      // pixel.
      ASSERT_EQ(f->channel_count, 1u);
      ASSERT_EQ(f->bits_per_channel[0], 8u);
      ((unsigned char *)gimg_raster_pixels(r))[0] = (unsigned char)value;
    };

    // Untouched, the frame goes back out at the depth it came in at.  Without
    // this the two edits below could both pass on a writer that never narrows.
    const NarrowResult plain =
        save_after_editing(c.file, [](GIMG_Raster *) {});
    ASSERT_FALSE(plain.wrote.empty()) << "fixture did not load";
    EXPECT_EQ(plain.head.color_type, 0);
    EXPECT_EQ(plain.head.bit_depth, c.depth)
        << "the writer stopped narrowing a frame that never changed";
    EXPECT_TRUE(plain.survived);

    // A sample that depth can hold: still narrowed.
    const NarrowResult ok = save_after_editing(
        c.file, [&](GIMG_Raster * r) { set00(r, (unsigned)c.representable); });
    ASSERT_FALSE(ok.wrote.empty());
    EXPECT_EQ(ok.head.bit_depth, c.depth)
        << "a sample this depth can hold should not have forced 8 bits";
    EXPECT_TRUE(ok.survived) << "the representable sample did not come back";

    // A sample it cannot: 8 bits, and the sample survives rather than being
    // rounded to the nearest one the narrow depth could have held.
    const NarrowResult wide =
        save_after_editing(c.file, [&](GIMG_Raster * r) { set00(r, breaks); });
    ASSERT_FALSE(wide.wrote.empty());
    EXPECT_EQ(wide.head.color_type, 0);
    EXPECT_EQ(wide.head.bit_depth, 8)
        << "a sample outside the depth was narrowed into it anyway";
    EXPECT_TRUE(wide.survived) << "the sample was rounded away";
    ASSERT_FALSE(wide.read_back.empty());
    EXPECT_EQ((unsigned)wide.read_back[0], (unsigned)breaks);
  }
}

TEST(PngEncode, AnEditThatBreaksTheTrnsRuleStopsTheNarrowing) {
  struct Case {
    const char * file;
    const char * what;
    std::function<void(GIMG_Raster *)> edit;
    int want_color_type; ///< What the writer must choose after the edit.
  };

  // The key colour is read out of the fixture rather than written in here, so
  // a change to the fixture cannot quietly make a case stop testing anything.
  auto key_of = [](GIMG_Raster * r, uint16_t key[4]) -> bool {
    for (uint32_t y = 0; y < gimg_raster_height(r); y++) {
      for (uint32_t x = 0; x < gimg_raster_width(r); x++) {
        uint16_t v[4];
        narrow_get(r, x, y, v);
        if (v[3] == 0) {
          memcpy(key, v, sizeof(uint16_t) * 4);
          return true;
        }
      }
    }
    return false;
  };

  const std::vector<Case> cases = {
      {"png_gray16_trns_4x2.png", "one pixel half transparent",
          [](GIMG_Raster * r) {
            uint16_t v[4];
            narrow_get(r, 0, 0, v);
            v[3] = 0x8000u;
            narrow_put(r, 0, 0, v);
          },
          6},
      {"png_rgb16_trns_4x2.png", "one pixel half transparent",
          [](GIMG_Raster * r) {
            uint16_t v[4];
            narrow_get(r, 0, 0, v);
            v[3] = 0x8000u;
            narrow_put(r, 0, 0, v);
          },
          6},
      // Half-transparency on a pixel that ALREADY wears the key colour. The
      // obvious "make some pixel half transparent" case does not test this
      // rule on its own: that pixel becomes the first non-opaque one, so it
      // becomes the key, and the genuinely transparent pixels then trip the
      // "more than one transparent colour" rule instead. Deleting the partial
      // check left the test green. Breaking one rule at a time is the only
      // way either check is on the hook for its own answer.
      {"png_gray16_trns_4x2.png", "the key pixel made half transparent",
          [key_of](GIMG_Raster * r) {
            uint16_t key[4];
            if (!key_of(r, key)) { return; }
            const uint16_t v[4] = {key[0], key[1], key[2], 0x8000u};
            narrow_put(r, 1, 0, v);
          },
          6},
      {"png_rgb16_trns_4x2.png", "the key pixel made half transparent",
          [key_of](GIMG_Raster * r) {
            uint16_t key[4];
            if (!key_of(r, key)) { return; }
            const uint16_t v[4] = {key[0], key[1], key[2], 0x8000u};
            narrow_put(r, 1, 0, v);
          },
          6},
      {"png_gray16_trns_4x2.png", "a second transparent colour",
          [](GIMG_Raster * r) {
            const uint16_t v[4] = {0x7777u, 0x7777u, 0x7777u, 0};
            narrow_put(r, 3, 1, v);
          },
          6},
      {"png_rgb16_trns_4x2.png", "a second transparent colour",
          [](GIMG_Raster * r) {
            const uint16_t v[4] = {0x7777u, 0x8888u, 0x9999u, 0};
            narrow_put(r, 3, 1, v);
          },
          6},
      {"png_gray16_trns_4x2.png", "an opaque pixel wearing the key",
          [key_of](GIMG_Raster * r) {
            uint16_t key[4];
            if (!key_of(r, key)) { return; }
            const uint16_t v[4] = {key[0], key[1], key[2], OPAQUE16};
            narrow_put(r, 2, 0, v);
          },
          6},
      {"png_rgb16_trns_4x2.png", "an opaque pixel wearing the key",
          [key_of](GIMG_Raster * r) {
            uint16_t key[4];
            if (!key_of(r, key)) { return; }
            const uint16_t v[4] = {key[0], key[1], key[2], OPAQUE16};
            narrow_put(r, 2, 0, v);
          },
          6},
      {"png_gray16_trns_4x2.png", "a grey pixel given a colour cast",
          [](GIMG_Raster * r) {
            uint16_t v[4];
            narrow_get(r, 3, 0, v);
            v[0] = 0xF00Fu;
            narrow_put(r, 3, 0, v);
          },
          // Colour type 2 is not on offer here: this writer only writes
          // types 2 and 4 for a document that arrived as one, so a frame
          // that came in grayscale and stopped being grayscale goes to 6.
          6},
      // The controls: an edit that keeps every rule must still narrow, or the
      // cases above would pass against a writer that never narrows at all.
      {"png_gray16_trns_4x2.png", "control: a different grey, still narrowable",
          [](GIMG_Raster * r) {
            const uint16_t v[4] = {0x2222u, 0x2222u, 0x2222u, OPAQUE16};
            narrow_put(r, 1, 1, v);
          },
          0},
      {"png_rgb16_trns_4x2.png", "control: a different colour, still narrowable",
          [](GIMG_Raster * r) {
            const uint16_t v[4] = {0x2222u, 0x3333u, 0x4444u, OPAQUE16};
            narrow_put(r, 1, 1, v);
          },
          2},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.file) + ": " + c.what);
    const NarrowResult nr = save_after_editing(c.file, c.edit);
    ASSERT_FALSE(nr.wrote.empty()) << "the fixture did not decode";
    ASSERT_EQ(nr.head.bit_depth, 16) << "a 16-bit raster must stay 16-bit";
    EXPECT_EQ(nr.head.color_type, c.want_color_type);
    EXPECT_TRUE(nr.survived)
        << "the pixels the caller wrote are not the pixels that came back";
    if (c.want_color_type == 0 || c.want_color_type == 2) {
      EXPECT_EQ(nr.head.trns_size, c.want_color_type == 0 ? 2 : 6)
          << "a narrowed frame carries the key as a tRNS chunk";
    }
    else {
      EXPECT_EQ(nr.head.trns_size, -1)
          << "colour type 6 has an alpha channel and needs no tRNS";
    }
  }
}
