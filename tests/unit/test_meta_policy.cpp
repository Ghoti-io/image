/**
 * @file
 *
 * A metadata policy is a promise about what a saved file contains, so each
 * one is checked in the bytes written, and against a control that shows the
 * data was there to remove.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <algorithm>
#include <gtest/gtest.h>
#include <vector>
#include "../../src/codec/webp/webp_internal.h"
#include "../exif_test_utils.h"

namespace {

typedef std::vector<uint8_t> Bytes;

/** The payload of the first @p fourcc chunk of a WebP file; empty if none. */
bool webp_chunk(const Bytes & f, const char * fourcc, Bytes & out) {
  for (size_t i = 12; i + 8 <= f.size();) {
    const size_t len = (size_t)f[i + 4] | ((size_t)f[i + 5] << 8) |
        ((size_t)f[i + 6] << 16) | ((size_t)f[i + 7] << 24);
    if (i + 8 + len > f.size()) {
      break;
    }
    if (memcmp(&f[i], fourcc, 4) == 0) {
      out.assign(f.begin() + (long)(i + 8), f.begin() + (long)(i + 8 + len));
      return true;
    }
    i += 8 + len + (len & 1u);
  }
  return false;
}

/** Save a 4x4 picture carrying @p exif and XMP as WebP under @p policy. */
GIMG_Result save_webp(const Bytes & exif, GIMG_Meta_Policy policy,
    Bytes & file) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                4, 4, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  memset(gimg_raster_pixels(r), 0xFF, gimg_raster_stride_bytes(r) * 4u);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Meta_Raw * raw = nullptr;
  EXPECT_EQ(gimg_doc_ensure_meta_raw(doc, &raw), GIMG_OK);
  EXPECT_NE(raw, nullptr);
  const char xmp[] = "<x:xmpmeta><exif:GPSLatitude>41,24.2N</exif:GPSLatitude>"
                     "</x:xmpmeta>";
  EXPECT_EQ(gimg_meta_raw_attach(
                raw, "webp", GIMG_WEBP_EXIF, exif.data(), exif.size()),
      GIMG_OK);
  EXPECT_EQ(gimg_meta_raw_attach(raw, "webp", GIMG_WEBP_XMP, xmp, sizeof xmp),
      GIMG_OK);

  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  GIMG_Result res = gimg_doc_save(doc, out, "webp", &opts, &report);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  file.assign((const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return res;
}

TEST(MetaPolicy, WebpStripGpsRemovesTheLocation) {
  const Bytes exif = exif_test::make_exif_with_gps();
  ASSERT_TRUE(exif_test::exif_has_gps_tag(exif));

  Bytes file, got, xmp;
  ASSERT_EQ(save_webp(exif, GIMG_META_PRESERVE_ALL, file), GIMG_OK);
  ASSERT_TRUE(webp_chunk(file, "EXIF", got)) << "control: Exif is written";
  EXPECT_TRUE(exif_test::exif_has_gps_tag(got)) << "control: with its GPS";
  EXPECT_TRUE(webp_chunk(file, "XMP ", xmp)) << "control: and the XMP";

  ASSERT_EQ(save_webp(exif, GIMG_META_STRIP_GPS, file), GIMG_OK);
  ASSERT_TRUE(webp_chunk(file, "EXIF", got)) << "the rest of the Exif stays";
  EXPECT_FALSE(exif_test::exif_has_gps_tag(got));
  EXPECT_EQ(exif_test::exif_ifd0_entry_count(got), 2);
  EXPECT_FALSE(webp_chunk(file, "XMP ", xmp))
      << "XMP can hold a location and is not parsed, so it is left out";
  const char needle[] = "GPSLatitude";
  EXPECT_EQ(std::search(file.begin(), file.end(), needle,
                needle + sizeof needle - 1),
      file.end());
}

TEST(MetaPolicy, WebpAnExifPolicyFailsOnExifItCannotParse) {
  const Bytes junk = {0, 1, 2, 3, 4, 5};
  Bytes file;
  EXPECT_EQ(save_webp(junk, GIMG_META_PRESERVE_ALL, file), GIMG_OK)
      << "control";
  EXPECT_NE(save_webp(junk, GIMG_META_STRIP_GPS, file), GIMG_OK);
  EXPECT_NE(save_webp(junk, GIMG_META_NORMALIZE_EXIF, file), GIMG_OK);
}

bool has_text(const Bytes & f, const char * needle) {
  return std::search(f.begin(), f.end(), needle, needle + strlen(needle)) !=
      f.end();
}

/** Save a described, credited, XMP-carrying 4x4 picture as TIFF. */
GIMG_Result save_tiff(GIMG_Meta_Policy policy, Bytes & file) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                4, 4, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Meta_Common * common = nullptr;
  EXPECT_EQ(gimg_doc_ensure_meta_common(doc, &common), GIMG_OK);
  EXPECT_NE(common, nullptr);
  if (common) {
    gimg_meta_common_set_description(common, "DESCRIBED-HERE");
  }
  GIMG_Meta_Raw * raw = nullptr;
  EXPECT_EQ(gimg_doc_ensure_meta_raw(doc, &raw), GIMG_OK);
  const char artist[] = "ARTIST-NAME";
  const char xmp[] = "<x:xmpmeta>XMP-GPS-41N</x:xmpmeta>";
  EXPECT_EQ(gimg_meta_raw_attach(raw, "tiff", 315u, artist, sizeof artist),
      GIMG_OK);
  EXPECT_EQ(gimg_meta_raw_attach(raw, "tiff", 700u, xmp, sizeof xmp - 1),
      GIMG_OK);
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  GIMG_Result res = gimg_doc_save(doc, out, "tiff", &opts, &report);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  file.assign((const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return res;
}

TEST(MetaPolicy, TiffWritesOnlyWhatThePolicyAllows) {
  struct Case {
    GIMG_Meta_Policy policy;
    bool description, artist, xmp;
  };
  const Case cases[] = {
      {GIMG_META_PRESERVE_ALL, true, true, true}, // the control
      {GIMG_META_DROP_ALL, false, false, false},
      {GIMG_META_KEEP_COMMON_ONLY, true, false, false},
      {GIMG_META_KEEP_RAW_ONLY, false, true, true},
      {GIMG_META_STRIP_GPS, true, true, false},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE((int)c.policy);
    Bytes file;
    ASSERT_EQ(save_tiff(c.policy, file), GIMG_OK);
    EXPECT_EQ(has_text(file, "DESCRIBED-HERE"), c.description);
    EXPECT_EQ(has_text(file, "ARTIST-NAME"), c.artist);
    EXPECT_EQ(has_text(file, "XMP-GPS-41N"), c.xmp);
  }
}

TEST(MetaPolicy, ARawBlockIsNotCopiedIntoABufferTooSmallForIt) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Meta_Raw * raw = nullptr;
  EXPECT_EQ(gimg_doc_ensure_meta_raw(doc, &raw), GIMG_OK);
  ASSERT_NE(raw, nullptr);
  const Bytes block(64, 0xAB);
  ASSERT_EQ(gimg_meta_raw_attach(raw, "x", 7, block.data(), block.size()),
      GIMG_OK);

  size_t size = 0;
  ASSERT_EQ(gimg_meta_raw_get(raw, "x", 7, nullptr, &size), GIMG_OK);
  EXPECT_EQ(size, 64u);

  Bytes small(16, 0x11);
  size = small.size();
  EXPECT_EQ(gimg_meta_raw_get(raw, "x", 7, small.data(), &size),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(size, 64u) << "the length needed";
  EXPECT_EQ(small, Bytes(16, 0x11)) << "nothing written";

  Bytes fits(64, 0);
  size = fits.size();
  EXPECT_EQ(gimg_meta_raw_get(raw, "x", 7, fits.data(), &size), GIMG_OK);
  EXPECT_EQ(fits, block);
  gimg_doc_destroy(doc);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
