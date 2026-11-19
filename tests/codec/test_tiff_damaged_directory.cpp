/**
 * @file
 *
 * A TIFF directory that cannot be read ends the document there instead of
 * refusing the file: the pages that did read are kept and a warning says what
 * was dropped.  The first directory must still read.  A reduced-resolution
 * item written back stays a reduced-resolution item.
 *
 * The files are built here, byte by byte, so each damage is exactly one fault.
 *
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

struct Page {
  uint32_t side = 4;
  bool reduced = false;     // NewSubfileType bit 0
  uint16_t compression = 1; // 99 is one this codec does not undo
  uint32_t sub_ifd = 0;     // value of a one-entry SubIFDs tag, 0 = none
  // What the next-IFD word holds: 0 = the following page (or 0 at the end),
  // otherwise this exact value.
  uint32_t next_override = 0;
  bool has_next_override = false;
};

static void p16(Bytes & b, uint16_t v) {
  b.push_back(v & 0xFF);
  b.push_back(v >> 8);
}
static void p32(Bytes & b, uint32_t v) {
  p16(b, v & 0xFFFF);
  p16(b, v >> 16);
}
static void entry(Bytes & b, uint16_t tag, uint16_t type, uint32_t value) {
  p16(b, tag);
  p16(b, type);
  p32(b, 1);
  if (type == 3) {
    p16(b, (uint16_t)value);
    p16(b, 0);
  }
  else {
    p32(b, value);
  }
}

/** Little-endian classic TIFF, one 8-bit grey strip per page. */
static Bytes build(const std::vector<Page> & pages) {
  Bytes b = {'I', 'I', 42, 0};
  p32(b, 8);
  for (size_t i = 0; i < pages.size(); i++) {
    const Page & pg = pages[i];
    const uint32_t entries = 10u + (pg.reduced ? 1u : 0u) + (pg.sub_ifd ? 1u : 0u);
    const uint32_t ifd_size = 2u + entries * 12u + 4u;
    const uint32_t data_at = (uint32_t)b.size() + ifd_size;
    const uint32_t bytes = pg.side * pg.side;
    p16(b, (uint16_t)entries);
    if (pg.reduced) {
      entry(b, 254, 4, 1);
    }
    entry(b, 256, 4, pg.side);
    entry(b, 257, 4, pg.side);
    entry(b, 258, 3, 8);
    entry(b, 259, 3, pg.compression);
    entry(b, 262, 3, 1);
    entry(b, 273, 4, data_at);
    entry(b, 277, 3, 1);
    entry(b, 278, 4, pg.side);
    entry(b, 279, 4, bytes);
    entry(b, 284, 3, 1);
    if (pg.sub_ifd) {
      entry(b, 330, 4, pg.sub_ifd);
    }
    const uint32_t next = pg.has_next_override ? pg.next_override
        : (i + 1 < pages.size() ? data_at + bytes : 0u);
    p32(b, next);
    for (uint32_t k = 0; k < bytes; k++) {
      b.push_back((uint8_t)(k + i));
    }
  }
  return b;
}

struct Loaded {
  GIMG_Doc * doc = nullptr;
  GIMG_Diagnostics diag = {};
  GIMG_Result r = GIMG_ERR_INTERNAL;
  Bytes keep;
  ~Loaded() {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    gimg_diagnostics_destroy(&diag);
  }
};

static void load(const Bytes & file, Loaded & out,
    const GIMG_Limits * limits = nullptr) {
  out.keep = file;
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(out.keep.data(), out.keep.size(), &s),
      GIMG_OK);
  GIMG_Load_Options opts = {};
  opts.limits = limits;
  out.r = gimg_doc_load(s, &opts, &out.diag, &out.doc);
  gimg_stream_destroy(s);
}

static size_t count_severity(const GIMG_Diagnostics & d, GIMG_Diag_Severity s) {
  size_t n = 0;
  for (size_t i = 0; i < d.count; i++) {
    n += d.items[i].severity == s;
  }
  return n;
}

static void expect_warned_not_errored(const Loaded & l) {
  EXPECT_GE(count_severity(l.diag, GIMG_DIAG_WARNING), 1u);
  EXPECT_EQ(count_severity(l.diag, GIMG_DIAG_ERROR), 0u);
}

static void expect_first_page_decodes(Loaded & l) {
  GIMG_Raster * px = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(l.doc, 0), nullptr, &px), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(px), 4u);
  gimg_raster_destroy(px);
}

} // namespace

TEST(TiffDamagedDirectory, ChainOffsetPastTheEndKeepsEarlierPages) {
  Page bad;
  bad.has_next_override = true;
  bad.next_override = 0x00FFFFF0u;
  Loaded l;
  load(build({Page{}, bad}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(l.doc), 2u); // the page that names it reads
  expect_warned_not_errored(l);
  expect_first_page_decodes(l);
}

TEST(TiffDamagedDirectory, TruncatedLaterDirectoryKeepsEarlierPages) {
  Bytes f = build({Page{}, Page{}});
  // Cut the file inside the second directory's entry table.
  const size_t second = 8 + 2 + 10 * 12 + 4 + 16;
  f.resize(second + 2 + 5 * 12);
  Loaded l;
  load(f, l);
  ASSERT_EQ(l.r, GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(l.doc), 1u);
  expect_warned_not_errored(l);
  expect_first_page_decodes(l);
}

TEST(TiffDamagedDirectory, UnsupportedLaterPageIsDroppedNotFatal) {
  Page odd;
  odd.compression = 99;
  Loaded l;
  load(build({Page{}, odd, Page{}}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(l.doc), 1u);
  expect_warned_not_errored(l);
  expect_first_page_decodes(l);
}

TEST(TiffDamagedDirectory, ChainThatLoopsBackEndsThere) {
  Page loop;
  loop.has_next_override = true;
  loop.next_override = 8; // the first directory again
  Loaded l;
  load(build({Page{}, loop}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(l.doc), 2u);
  expect_warned_not_errored(l);
}

TEST(TiffDamagedDirectory, FirstDirectoryUnreadableStaysFatal) {
  Page bad;
  bad.compression = 99;
  Loaded l;
  load(build({bad, Page{}}), l);
  EXPECT_EQ(l.r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(l.doc, nullptr);
  EXPECT_GE(count_severity(l.diag, GIMG_DIAG_ERROR), 1u);

  Bytes f = build({Page{}});
  f[4] = 0xF0;
  f[5] = 0xFF;
  f[6] = 0xFF;
  f[7] = 0x00; // header points past the end
  Loaded m;
  load(f, m);
  EXPECT_EQ(m.r, GIMG_ERR_CORRUPT);
  EXPECT_EQ(m.doc, nullptr);
}

TEST(TiffDamagedDirectory, DamagedSubIfdIsDroppedAndTheChainContinues) {
  Page with_sub;
  with_sub.sub_ifd = 0x00FFFFF0u;
  Loaded l;
  load(build({with_sub, Page{}}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(l.doc), 2u); // both pages, no level
  EXPECT_EQ(gimg_item_role(gimg_doc_item(l.doc, 1)), GIMG_ITEM_IMAGE);
  expect_warned_not_errored(l);
}

TEST(TiffDamagedDirectory, LimitsStillRefuseTheFile) {
  GIMG_Limits limits;
  gimg_limits_default(&limits);
  limits.max_frame_count = 1;
  Loaded l;
  load(build({Page{}, Page{}}), l, &limits);
  EXPECT_EQ(l.r, GIMG_ERR_LIMIT);
  EXPECT_EQ(l.doc, nullptr);
}

TEST(TiffDamagedDirectory, ASubIfdNamingItsOwnPageIsDropped) {
  // A SubIFD naming its own page cannot be a level of itself: it is dropped
  // with a warning, and the page it hangs off is kept.
  Page self;
  self.sub_ifd = 8;
  Loaded l;
  load(build({self}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(l.doc), 1u);
  expect_warned_not_errored(l);
}

TEST(TiffPyramidWrite, ALevelIsWrittenBackAsALevel) {
  Page level;
  level.side = 2;
  level.reduced = true;
  Loaded l;
  load(build({Page{}, level}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(l.doc), 2u);
  ASSERT_EQ(gimg_item_role(gimg_doc_item(l.doc, 1)), GIMG_ITEM_LEVEL);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(l.doc, out, "tiff", nullptr, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);

  Loaded again;
  load(Bytes((const uint8_t *)data, (const uint8_t *)data + size), again);
  gimg_stream_destroy(out);
  ASSERT_EQ(again.r, GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(again.doc), 2u);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(again.doc, 0)), GIMG_ITEM_IMAGE);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(again.doc, 1)), GIMG_ITEM_LEVEL);
  EXPECT_EQ(gimg_item_role_subject(gimg_doc_item(again.doc, 1)), 0u);
}

TEST(TiffPyramidWrite, ThePlainPagesStayPictures) {
  Loaded l;
  load(build({Page{}, Page{}}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(l.doc, out, "tiff", nullptr, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  Loaded again;
  load(Bytes((const uint8_t *)data, (const uint8_t *)data + size), again);
  gimg_stream_destroy(out);
  ASSERT_EQ(again.r, GIMG_OK);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(again.doc, 1)), GIMG_ITEM_IMAGE);
}

TEST(TiffPyramidWrite, CapabilitiesIncludeIcc) {
  const GIMG_Codec * c = gimg_codec_by_name("tiff");
  ASSERT_NE(c, nullptr);
  EXPECT_TRUE(gimg_codec_capabilities(c) & GIMG_CAP_ICC);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
