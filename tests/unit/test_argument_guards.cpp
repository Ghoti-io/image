/**
 * @file
 *
 * What the public API does when it is handed something it cannot use.
 *
 * Every function here refuses bad arguments, and none of those refusals was
 * reached by any test: 54 guard sites across ten files, every one of them a
 * documented part of the contract and none of them exercised. A guard nothing
 * calls is a guard nobody knows the shape of - it can return the wrong code,
 * or write through the pointer it was checking, and the suite stays green.
 *
 * Three things are asserted, not one. That the call refuses; that it refuses
 * with the code the header promises; and - where the function has an output
 * parameter - that it left the caller's memory alone. The third is the half
 * that a `!= GIMG_OK` check on its own cannot see, and is why several of these
 * pass a poisoned pointer rather than a null one.
 *
 * `GIMG_ERR_INTERNAL` is this library's code for "the caller broke the
 * contract" rather than "something went wrong inside", which reads oddly
 * until you see it used: a null out-parameter is a programming error, and the
 * library says so rather than inventing a result the caller might handle.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include <ghoti.io/image/image.h>

namespace {

/** A byte pattern no valid call would leave behind. */
const void * const kPoison = reinterpret_cast<const void *>(0xD15EA5EULL);

/** A raster to hand to functions that need a real one. */
GIMG_Raster * MakeRaster(uint32_t w = 4, uint32_t h = 4) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr,
          0, &r) != GIMG_OK) {
    return nullptr;
  }
  return r;
}

} // namespace

// ---------------------------------------------------------------- streams

TEST(ArgumentGuards, MemoryStreamConstructorsRefuseANullOutParameter) {
  const unsigned char data[4] = {1, 2, 3, 4};
  EXPECT_EQ(gimg_stream_create_memory(data, sizeof(data), nullptr),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_create_memory_no_seek(data, sizeof(data), nullptr),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(
      gimg_stream_create_memory_chunked(data, sizeof(data), 2, nullptr),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_create_memory_no_seek_chunked(
                data, sizeof(data), 2, nullptr),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_create_memory_output(nullptr), GIMG_ERR_INTERNAL);
}

TEST(ArgumentGuards, AMemoryStreamOverNothingIsOnlyValidWhenItIsEmpty) {
  // Null with a non-zero size is a caller error; null with size zero is an
  // empty stream, which is a legitimate thing to read nothing from. The two
  // sit one line apart in every one of these four constructors, so the empty
  // case is asserted beside the refusal rather than assumed.
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(nullptr, 1, &s), GIMG_ERR_INTERNAL);
  EXPECT_EQ(s, nullptr) << "a refused constructor must not hand back a stream";
  EXPECT_EQ(gimg_stream_create_memory_no_seek(nullptr, 1, &s),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_create_memory_chunked(nullptr, 1, 2, &s),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_create_memory_no_seek_chunked(nullptr, 1, 2, &s),
      GIMG_ERR_INTERNAL);

  ASSERT_EQ(gimg_stream_create_memory(nullptr, 0, &s), GIMG_OK)
      << "size zero names an empty stream, not a missing buffer";
  ASSERT_NE(s, nullptr);
  unsigned char byte = 0;
  size_t got = 99;
  EXPECT_EQ(gimg_stream_read(s, &byte, 1, &got), GIMG_OK);
  EXPECT_EQ(got, 0u) << "an empty stream reads zero bytes, and says so";
  gimg_stream_destroy(s);
}

TEST(ArgumentGuards, StreamOperationsRefuseANullStreamOrBuffer) {
  unsigned char buf[4] = {0, 0, 0, 0};
  size_t n = 12345;

  EXPECT_EQ(gimg_stream_read(nullptr, buf, 4, &n), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_read_exact(nullptr, buf, 4), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_peek(nullptr, buf, 4, &n), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_skip(nullptr, 4, &n), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_seek(nullptr, 0), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_write(nullptr, buf, 4, &n), GIMG_ERR_INTERNAL);
  EXPECT_EQ(n, 12345u)
      << "a refused call must not write through the out-parameter it rejected";

  const unsigned char data[4] = {1, 2, 3, 4};
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, sizeof(data), &s), GIMG_OK);
  EXPECT_EQ(gimg_stream_read(s, nullptr, 4, &n), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_read(s, buf, 4, nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_read_exact(s, nullptr, 4), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_peek(s, nullptr, 4, &n), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_peek(s, buf, 4, nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_stream_skip(s, 4, nullptr), GIMG_ERR_INTERNAL);
  gimg_stream_destroy(s);
}

TEST(ArgumentGuards, WritingToAReadOnlyStreamIsUnsupportedRatherThanInternal) {
  // The distinction is the contract: a read-only stream is a perfectly valid
  // object being asked for something it does not do, which is UNSUPPORTED; a
  // null out-parameter is the caller's mistake, which is INTERNAL. The two
  // refusals are four lines apart in gimg_stream_write.
  const unsigned char data[4] = {1, 2, 3, 4};
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, sizeof(data), &s), GIMG_OK);
  size_t n = 7;
  EXPECT_EQ(gimg_stream_write(s, data, 4, &n), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(n, 0u) << "the count is cleared before the writability check";
  gimg_stream_destroy(s);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  EXPECT_EQ(gimg_stream_write(out, nullptr, 4, &n), GIMG_ERR_INTERNAL)
      << "a null buffer with a non-zero size is the caller's error";
  EXPECT_EQ(gimg_stream_write(out, nullptr, 0, &n), GIMG_OK)
      << "writing nothing is a no-op, and needs no buffer to do it with";
  gimg_stream_destroy(out);
}

TEST(ArgumentGuards, TheVoidReturningStreamCallsSurviveANullArgument) {
  // Nothing to assert but the absence of a crash, which is the whole point:
  // these have no way to report a refusal, so the guard is the contract.
  gimg_stream_destroy(nullptr);

  const void * data = kPoison;
  size_t size = 4242;
  gimg_stream_output_buffer(nullptr, &data, &size);
  EXPECT_EQ(data, kPoison) << "a refused call must not write its outputs";
  EXPECT_EQ(size, 4242u);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  gimg_stream_output_buffer(out, nullptr, &size);
  gimg_stream_output_buffer(out, &data, nullptr);
  EXPECT_EQ(data, kPoison);
  EXPECT_EQ(size, 4242u);
  gimg_stream_destroy(out);

  gimg_limits_default(nullptr);
}

// ------------------------------------------------------------------ colour

TEST(ArgumentGuards, ColorInfoDefaultSurvivesANullArgument) {
  gcol_color_info_default(nullptr);

  // And fills the struct when given one, so the guard is not the only path
  // this test can reach.
  GCOL_Color_Info info;
  std::memset(&info, 0xA5, sizeof(info));
  gcol_color_info_default(&info);
  EXPECT_EQ(info.cmyk_polarity, GCOL_CMYK_POLARITY_UNKNOWN);
}

// --------------------------------------------------------------- documents

TEST(ArgumentGuards, DocumentConstructorsRefuseANullOutParameter) {
  EXPECT_EQ(gimg_doc_create(nullptr), GIMG_ERR_INTERNAL);

  GIMG_Raster * raster = MakeRaster();
  ASSERT_NE(raster, nullptr);
  EXPECT_EQ(gimg_doc_from_raster(raster, nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_doc_from_raster(nullptr, nullptr), GIMG_ERR_INTERNAL);

  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_copy(nullptr, &doc), GIMG_ERR_INTERNAL);
  EXPECT_EQ(doc, nullptr);
  gimg_raster_destroy(raster);
}

TEST(ArgumentGuards, DocumentAccessorsRefuseANullDocumentOrItem) {
  EXPECT_EQ(gimg_doc_set_item_count(nullptr, 1), GIMG_ERR_INTERNAL);

  GIMG_Meta_Common * common = nullptr;
  GIMG_Meta_Raw * raw = nullptr;
  EXPECT_EQ(gimg_doc_ensure_meta_common(nullptr, &common), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_doc_ensure_meta_raw(nullptr, &raw), GIMG_ERR_INTERNAL);
  EXPECT_EQ(common, nullptr);
  EXPECT_EQ(raw, nullptr);

  EXPECT_EQ(gimg_item_copy(nullptr, nullptr), GIMG_ERR_INTERNAL);

  // A void setter and a void getter, neither of which can report anything.
  gimg_item_set_raster(nullptr, nullptr);

  // Not a refusal: a null item has no delay, and saying so by zeroing the
  // outputs is more useful than leaving the caller's stack as it found it.
  // Asserted because it is a contract either way, and the first draft of this
  // test asserted the opposite and failed.
  uint16_t num = 111, den = 222;
  gimg_item_frame_delay(nullptr, &num, &den);
  EXPECT_EQ(num, 0) << "no item means no delay, stated rather than implied";
  EXPECT_EQ(den, 0);
  gimg_item_frame_delay(nullptr, nullptr, nullptr);
}

// --------------------------------------------------------------- metadata

TEST(ArgumentGuards, MetadataObjectsRefuseANullOutParameterOrSubject) {
  EXPECT_EQ(gimg_meta_common_create(nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_meta_raw_create(nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_meta_common_set_description(nullptr, "x"),
      GIMG_ERR_INTERNAL);

  gimg_meta_common_destroy(nullptr);
  gimg_meta_raw_destroy(nullptr);

  GIMG_Meta_Raw * raw = nullptr;
  ASSERT_EQ(gimg_meta_raw_create(&raw), GIMG_OK);
  ASSERT_NE(raw, nullptr);
  const unsigned char blob[2] = {0xAB, 0xCD};
  EXPECT_EQ(gimg_meta_raw_attach(nullptr, "exif", 0, blob, sizeof(blob)),
      GIMG_ERR_INTERNAL);

  unsigned char out[4] = {0, 0, 0, 0};
  size_t size = 4242;
  EXPECT_EQ(gimg_meta_raw_get(nullptr, "exif", 0, out, &size),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(size, 4242u)
      << "a refused call must not write through the size it rejected";

  // Same shape again: copying nothing succeeds and yields nothing, and only a
  // null *destination* is the caller's error. The two lines sit three apart.
  GIMG_Meta_Raw * copy = reinterpret_cast<GIMG_Meta_Raw *>(1);
  EXPECT_EQ(gimg_meta_raw_copy(nullptr, &copy), GIMG_OK)
      << "a copy of no metadata is no metadata, not a failure";
  EXPECT_EQ(copy, nullptr) << "and it must say so rather than leave the "
                              "caller's pointer where it was";
  EXPECT_EQ(gimg_meta_raw_copy(raw, nullptr), GIMG_ERR_INTERNAL);
  gimg_meta_raw_destroy(raw);
}

// ---------------------------------------------------------------- rasters

TEST(ArgumentGuards, RasterCreationRefusesEveryShapeItCannotBuild) {
  GIMG_Raster * r = nullptr;
  const GIMG_Pixel_Format * fmt = &GIMG_PIXEL_RGBA8;
  EXPECT_EQ(
      gimg_raster_create(4, 4, fmt, GIMG_RASTER_OWNED, nullptr, 0, nullptr),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_raster_create(0, 4, fmt, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(r, nullptr) << "a refused creation must hand back nothing";
  EXPECT_EQ(gimg_raster_create(4, 0, fmt, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_raster_create(4, 4, nullptr, GIMG_RASTER_OWNED, nullptr, 0,
                &r),
      GIMG_ERR_INTERNAL);

  // Ownership is an enum with two members and a COUNT sentinel, so a value
  // outside it is a caller error rather than a third behaviour.
  EXPECT_EQ(gimg_raster_create(4, 4, fmt, GIMG_RASTER_OWNERSHIP_COUNT, nullptr,
                0, &r),
      GIMG_ERR_INTERNAL);
  // BORROWED names a buffer the caller supplies, so it cannot be absent.
  EXPECT_EQ(
      gimg_raster_create(4, 4, fmt, GIMG_RASTER_BORROWED, nullptr, 0, &r),
      GIMG_ERR_INTERNAL);
  // A stride narrower than one row cannot hold the row it claims to.
  std::vector<uint8_t> buffer(4 * 4 * 4);
  EXPECT_EQ(gimg_raster_create(4, 4, fmt, GIMG_RASTER_BORROWED, buffer.data(),
                4, &r),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(r, nullptr);
}

TEST(ArgumentGuards, RasterAccessorsRefuseANullSubject) {
  GIMG_Raster * copy = nullptr;
  EXPECT_EQ(gimg_raster_copy(nullptr, &copy), GIMG_ERR_INTERNAL);
  EXPECT_EQ(copy, nullptr);

  GIMG_Raster * src = MakeRaster();
  ASSERT_NE(src, nullptr);
  EXPECT_EQ(gimg_raster_copy(src, nullptr), GIMG_ERR_INTERNAL);

  GCOL_Color_Info info;
  gcol_color_info_default(&info);
  EXPECT_EQ(gimg_raster_set_color_info(nullptr, &info), GIMG_ERR_INTERNAL);

  GIMG_Pixel_Format fmt;
  EXPECT_EQ(gimg_pixel_format_multichannel(0, 8, &fmt), GIMG_ERR_UNSUPPORTED)
      << "zero channels names no format";
  EXPECT_EQ(gimg_pixel_format_multichannel(3, 8, nullptr),
      GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(gimg_pixel_format_multichannel(3, 7, &fmt), GIMG_ERR_UNSUPPORTED)
      << "the depth must be one this library stores, not any byte";
  gimg_raster_destroy(src);
}

// -------------------------------------------------------------- operations

TEST(ArgumentGuards, OperationsRefuseANullRasterOrOutParameter) {
  GIMG_Raster * src = MakeRaster(8, 8);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * out = nullptr;

  EXPECT_EQ(gimg_ops_convert_pixel_format(nullptr, &GIMG_PIXEL_RGBA8, &out),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_convert_pixel_format(src, nullptr, &out),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, nullptr),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(out, nullptr);

  EXPECT_EQ(gimg_ops_convert_bit_depth(nullptr, 8, &out), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_convert_bit_depth(src, 8, nullptr), GIMG_ERR_INTERNAL);

  EXPECT_EQ(gimg_ops_apply_orientation(nullptr, GIMG_ORIENTATION_NORMAL),
      GIMG_ERR_INTERNAL);

  EXPECT_EQ(gimg_alpha_premultiply(nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_alpha_unpremultiply(nullptr), GIMG_ERR_INTERNAL);

  EXPECT_EQ(gimg_ops_crop(nullptr, 0, 0, 1, 1, &out), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_crop(src, 0, 0, 1, 1, nullptr), GIMG_ERR_INTERNAL);

  EXPECT_EQ(gimg_ops_composite(nullptr, src, 0, 0, GIMG_COMPOSITE_OVER),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_composite(src, nullptr, 0, 0, GIMG_COMPOSITE_OVER),
      GIMG_ERR_INTERNAL);

  gimg_raster_destroy(src);
}

TEST(ArgumentGuards, ResizeRefusesANullArgumentAndADegenerateSize) {
  GIMG_Raster * src = MakeRaster(8, 8);
  ASSERT_NE(src, nullptr);
  GIMG_Resize_Options opts;
  gimg_resize_options_default(&opts);
  GIMG_Raster * out = nullptr;

  EXPECT_EQ(gimg_ops_resize(nullptr, 4, 4, &opts, &out), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_resize(src, 4, 4, &opts, nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_resize(src, 0, 4, &opts, &out), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_resize(src, 4, 0, &opts, &out), GIMG_ERR_INTERNAL);
  EXPECT_EQ(out, nullptr) << "a refused resize must hand back nothing";

  gimg_resize_options_default(nullptr);
  gimg_raster_destroy(src);
}

// -------------------------------------------------------------- the codecs

TEST(ArgumentGuards, TheCodecEntryPointsRefuseANullArgument) {
  const unsigned char data[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, sizeof(data), &s), GIMG_OK);

  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_load(nullptr, nullptr, nullptr, &doc), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_doc_load(s, nullptr, nullptr, nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(doc, nullptr);

  GIMG_Probe_Result probe;
  probe.format_name = static_cast<const char *>(kPoison);
  probe.confidence = 4242;
  EXPECT_EQ(gimg_probe(nullptr, &probe), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_probe(s, nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(probe.format_name, static_cast<const char *>(kPoison))
      << "a refused probe must not write its out-parameter";
  EXPECT_EQ(probe.confidence, 4242u);

  GIMG_Raster * raster = nullptr;
  EXPECT_EQ(gimg_item_decode(nullptr, nullptr, &raster), GIMG_ERR_INTERNAL);
  EXPECT_EQ(raster, nullptr);
  gimg_stream_destroy(s);
}

TEST(ArgumentGuards, TheAbbreviatedTableLoaderRefusesANullArgument) {
  GIMG_JPEG_Tables * tables = nullptr;
  const unsigned char data[4] = {0xFF, 0xD8, 0xFF, 0xD9};
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, sizeof(data), &s), GIMG_OK);

  EXPECT_EQ(gimg_jpeg_tables_load(nullptr, &tables), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_jpeg_tables_load(s, nullptr), GIMG_ERR_INTERNAL);
  EXPECT_EQ(tables, nullptr);
  gimg_jpeg_tables_destroy(nullptr);
  gimg_stream_destroy(s);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
