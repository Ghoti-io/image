/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * A save that cannot carry something the document holds says so.
 *
 * Saving a document loaded from one format as another used to drop whatever
 * the new format could not hold - Exif, XMP, the loop count, a cursor hotspot -
 * and return GIMG_OK with nothing to show for it.  GIMG_Save_Report.diagnostics
 * now receives one warning per kind of metadata that is in the document, is not
 * in the file, and was not removed on the caller's request.
 *
 * The oracle is the file.  For every document, writer and metadata policy the
 * saved bytes are read back, and each kind is classified as present or absent
 * there.  A warning must name a kind that the document held and the file does
 * not (no false alarm), and a kind the document held that the file lacks must
 * be warned about unless the policy asked for its removal (no silent loss).
 * On top of that, the exact warnings for the document that carries everything
 * are listed per writer, so a reworded or re-keyed message is a failure here.
 *
 * Nothing about what is written changes: a save with no diagnostics supplied
 * must produce the same bytes as one with, and the writers whose output does
 * not depend on floating point are held to bytes recorded before the report
 * existed.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "../exif_test_utils.h"

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Kinds, as the warnings name them
// ---------------------------------------------------------------------------

enum Kind {
  EXIF,
  XMP,
  ICC,
  RES,
  DESC,
  COMMENTS,
  TEXT,
  NATIVE,
  LOOP,
  DELAY,
  HOTSPOT,
  ROLES,
  ITEMS,
  KIND_COUNT
};

const char * const kind_prefix[KIND_COUNT] = {"Exif dropped", "XMP dropped",
    "ICC profile dropped", "Resolution dropped", "Description dropped",
    "Additional comments dropped", "Text fields dropped",
    "Format-specific metadata dropped", "Loop count dropped",
    "Frame delays dropped", "Cursor hotspot dropped", "Item roles dropped",
    "Items dropped"};

const char * const kind_name[KIND_COUNT] = {"exif", "xmp", "icc", "res", "desc",
    "comments", "text", "native", "loop", "delay", "hotspot", "roles", "items"};

const char * const writers[] = {
    "jpeg", "png", "tiff", "webp", "gif", "bmp", "ico"};
constexpr size_t WRITER_COUNT = sizeof(writers) / sizeof(writers[0]);

const GIMG_Meta_Policy policies[] = {GIMG_META_PRESERVE_ALL,
    GIMG_META_DROP_ALL, GIMG_META_STRIP_GPS, GIMG_META_NORMALIZE_EXIF,
    GIMG_META_KEEP_RAW_ONLY, GIMG_META_KEEP_COMMON_ONLY};
const char * const policy_name[] = {"PRESERVE_ALL", "DROP_ALL", "STRIP_GPS",
    "NORMALIZE_EXIF", "KEEP_RAW_ONLY", "KEEP_COMMON_ONLY"};

int kind_of(const std::string & message) {
  for (int k = 0; k < KIND_COUNT; k++) {
    if (message.rfind(kind_prefix[k], 0) == 0) {
      return k;
    }
  }
  return -1;
}

using Set = std::vector<bool>;

Set no_kinds() {
  return Set(KIND_COUNT, false);
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

constexpr const char * MARK_XMP = "MARKXMP";
constexpr const char * MARK_DESC = "MARKDESC";
constexpr const char * MARK_COM2 = "MARKCOM2";
constexpr const char * MARK_GIF2 = "MARKGIF2";
constexpr const char * MARK_TEXT = "MARKTEXT";
constexpr const char * MARK_IPTC = "MARKIPTC";

Bytes profile_bytes() {
  Bytes p(128, 0);
  p[3] = 128;
  std::memcpy(p.data() + 36, "acsp", 4);
  for (size_t i = 40; i < p.size(); i++) {
    p[i] = static_cast<uint8_t>((i * 7u) & 0xFFu);
  }
  return p;
}

GIMG_Raster * make_raster() {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      uint8_t * p = px + y * stride + x * 4u;
      p[0] = static_cast<uint8_t>(x * 30u);
      p[1] = static_cast<uint8_t>(y * 30u);
      p[2] = static_cast<uint8_t>(100u + x + y);
      p[3] = 255u;
    }
  }
  const Bytes prof = profile_bytes();
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = prof.data();
  ci.icc_size = prof.size();
  EXPECT_EQ(gimg_raster_set_color_info(r, &ci), GIMG_OK);
  return r;
}

void attach(GIMG_Meta_Raw * raw, const char * fmt, uint32_t id, const Bytes & b) {
  ASSERT_EQ(gimg_meta_raw_attach(raw, fmt, id, b.data(), b.size()), GIMG_OK);
}

Bytes text_bytes(const std::string & s) {
  return Bytes(s.begin(), s.end());
}

Bytes framed(const std::vector<std::string> & parts, size_t prefix) {
  Bytes out;
  for (const auto & s : parts) {
    for (size_t i = 0; i < prefix; i++) {
      out.push_back(static_cast<uint8_t>(s.size() >> (8u * (prefix - 1u - i))));
    }
    out.insert(out.end(), s.begin(), s.end());
  }
  return out;
}

Bytes jpeg_exif_payload() {
  Bytes p = {'E', 'x', 'i', 'f', 0, 0};
  const Bytes t = exif_test::make_exif_with_gps();
  p.insert(p.end(), t.begin(), t.end());
  return p;
}

constexpr uint32_t webp_fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
      (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
      (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
      (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

constexpr uint32_t PNG_EXIF = 0x65584966u; // 'eXIf'
constexpr uint32_t JPEG_EXIF = 0xE100u;
constexpr uint32_t JPEG_XMP = 0xE101u;
constexpr uint32_t JPEG_COM = 0xFEu;
constexpr uint32_t JPEG_APP13 = 0xEDu;
constexpr uint32_t GIF_COMMENT = 0xFEu;

/** Which families of raw block a synthetic document carries. */
struct Spec {
  bool jpeg = false, png = false, webp = false, tiff = false, gif = false;
  bool junk = false;
  bool animated = false;
  bool common = true; ///< Resolution and description.
};

/** What a synthetic document holds, as kinds (the oracle for the source). */
Set holds(const Spec & s) {
  Set k = no_kinds();
  k[ICC] = true;
  k[RES] = s.common;
  k[DESC] = s.common;
  k[EXIF] = s.jpeg || s.png || s.webp;
  k[XMP] = s.jpeg || s.webp || s.tiff;
  k[COMMENTS] = s.jpeg || s.gif;
  k[TEXT] = s.tiff;
  k[NATIVE] = s.jpeg || s.junk;
  k[LOOP] = s.animated;
  k[DELAY] = s.animated;
  k[HOTSPOT] = s.animated;
  k[ROLES] = s.animated;
  k[ITEMS] = s.animated;
  return k;
}

GIMG_Doc * make_doc(const Spec & s) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, s.animated ? 2 : 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), make_raster());
  if (s.animated) {
    gimg_item_set_raster(gimg_doc_item(doc, 1), make_raster());
    gimg_item_set_role(gimg_doc_item(doc, 1), GIMG_ITEM_THUMBNAIL, 0);
    gimg_item_set_frame_delay(gimg_doc_item(doc, 0), 10, 100);
    gimg_item_set_frame_delay(gimg_doc_item(doc, 1), 10, 100);
    gimg_item_set_hotspot(gimg_doc_item(doc, 0), 3, 2);
    gimg_doc_set_loop_count(doc, 3);
  }
  if (s.common) {
    GIMG_Meta_Common * common = nullptr;
    EXPECT_EQ(gimg_doc_ensure_meta_common(doc, &common), GIMG_OK);
    gimg_meta_common_set_dpi(common, 300, 300);
    EXPECT_EQ(gimg_meta_common_set_description(common, MARK_DESC), GIMG_OK);
  }
  GIMG_Meta_Raw * raw = nullptr;
  EXPECT_EQ(gimg_doc_ensure_meta_raw(doc, &raw), GIMG_OK);
  if (s.jpeg) {
    attach(raw, "jpeg", JPEG_EXIF, jpeg_exif_payload());
    attach(raw, "jpeg", JPEG_XMP,
        text_bytes(std::string("http://ns.adobe.com/xap/1.0/") + '\0' +
            "<x:xmpmeta>" + MARK_XMP + "</x:xmpmeta>"));
    attach(raw, "jpeg", JPEG_COM, framed({MARK_DESC, MARK_COM2}, 2));
    attach(raw, "jpeg", JPEG_APP13, text_bytes(std::string("Photoshop 3.0") + '\0' + MARK_IPTC));
  }
  if (s.png) {
    attach(raw, "png", PNG_EXIF, exif_test::make_exif_with_gps());
  }
  if (s.webp) {
    attach(raw, "webp", webp_fourcc('E', 'X', 'I', 'F'),
        exif_test::make_exif_with_gps());
    attach(raw, "webp", webp_fourcc('X', 'M', 'P', ' '),
        text_bytes(std::string("<x:xmpmeta>") + MARK_XMP + "</x:xmpmeta>"));
  }
  if (s.tiff) {
    attach(raw, "tiff", 700,
        text_bytes(std::string("<x:xmpmeta>") + MARK_XMP + "</x:xmpmeta>"));
    attach(raw, "tiff", 315, text_bytes(std::string(MARK_TEXT) + '\0'));
  }
  if (s.gif) {
    attach(raw, "gif", GIF_COMMENT, framed({MARK_DESC, MARK_GIF2}, 4));
  }
  if (s.junk) {
    attach(raw, "png", webp_fourcc('t', 'E', 'X', 't'), text_bytes("junk"));
    attach(raw, "private", 7, text_bytes("junk"));
  }
  return doc;
}

// ---------------------------------------------------------------------------
// Saving and reading back
// ---------------------------------------------------------------------------

struct Saved {
  GIMG_Result result = GIMG_ERR_INTERNAL;
  Bytes bytes;
  std::vector<std::string> warnings;
  std::vector<std::string> warning_codecs;
};

Saved save(const GIMG_Doc * doc, const char * format, GIMG_Meta_Policy policy,
    bool with_diagnostics) {
  Saved s;
  GIMG_Save_Options opts;
  std::memset(&opts, 0, sizeof(opts));
  opts.metadata_policy = policy;
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  GIMG_Diagnostics diag;
  std::memset(&diag, 0, sizeof(diag));
  gimg_diagnostics_init(&diag, nullptr);
  if (with_diagnostics) {
    report.diagnostics = &diag;
  }
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  s.result = gimg_doc_save(doc, out, format, &opts, &report);
  if (s.result == GIMG_OK) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out, &p, &n);
    s.bytes.assign(static_cast<const uint8_t *>(p),
        static_cast<const uint8_t *>(p) + n);
    EXPECT_EQ(report.bytes_written, n) << format;
  }
  gimg_stream_destroy(out);
  for (size_t i = 0; i < diag.count; i++) {
    EXPECT_EQ(diag.items[i].severity, GIMG_DIAG_WARNING);
    s.warnings.push_back(diag.items[i].recommended_action);
    s.warning_codecs.push_back(diag.items[i].codec_name);
  }
  gimg_diagnostics_destroy(&diag);
  return s;
}

struct Loaded {
  Bytes bytes;
  GIMG_Stream * stream = nullptr;
  GIMG_Doc * doc = nullptr;
  ~Loaded() {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    if (stream) {
      gimg_stream_destroy(stream);
    }
  }
  GIMG_Result load(const Bytes & b) {
    bytes = b;
    GIMG_Result r = gimg_stream_create_memory(bytes.data(), bytes.size(), &stream);
    if (r != GIMG_OK) {
      return r;
    }
    return gimg_doc_load(stream, nullptr, nullptr, &doc);
  }
};

bool contains(const Bytes & b, const char * needle) {
  const size_t n = std::strlen(needle);
  for (size_t i = 0; i + n <= b.size(); i++) {
    if (std::memcmp(&b[i], needle, n) == 0) {
      return true;
    }
  }
  return false;
}

bool has_raw(const GIMG_Doc * doc, const char * fmt, uint32_t id) {
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  size_t n = 0;
  return raw && gimg_meta_raw_get(raw, fmt, id, nullptr, &n) == GIMG_OK && n > 0;
}

/** What a file holds, read back through the library and from its bytes. */
Set present_in(const Bytes & file) {
  Set k = no_kinds();
  Loaded l;
  if (l.load(file) != GIMG_OK) {
    ADD_FAILURE() << "an output file did not load";
    return k;
  }
  const GIMG_Doc * doc = l.doc;
  k[ICC] = false;
  k[EXIF] = has_raw(doc, "jpeg", JPEG_EXIF) || has_raw(doc, "png", PNG_EXIF) ||
      has_raw(doc, "webp", webp_fourcc('E', 'X', 'I', 'F'));
  k[XMP] = contains(file, MARK_XMP);
  GIMG_Item * first = gimg_doc_item(doc, 0);
  GIMG_Raster * raster = nullptr;
  if (first && gimg_item_decode(first, nullptr, &raster) == GIMG_OK && raster) {
    const GCOL_Color_Info * ci = gimg_raster_color_info_const(raster);
    k[ICC] = ci && ci->icc_bytes && ci->icc_size > 0;
    gimg_raster_destroy(raster);
  }
  // A WebP carries its profile in an ICCP chunk, which the decoder leaves in
  // the document's raw blocks rather than on the raster.
  if (has_raw(doc, "webp", webp_fourcc('I', 'C', 'C', 'P'))) {
    k[ICC] = true;
  }
  GIMG_Meta_Common * common = gimg_doc_meta_common(doc);
  if (common) {
    uint32_t x = 0, y = 0;
    gimg_meta_common_dpi(common, &x, &y);
    k[RES] = x > 0 && y > 0;
  }
  k[DESC] = contains(file, MARK_DESC);
  k[COMMENTS] = contains(file, MARK_COM2) || contains(file, MARK_GIF2);
  k[TEXT] = contains(file, MARK_TEXT) || contains(file, "tEXtAuthor");
  k[NATIVE] = contains(file, MARK_IPTC) || contains(file, "tIME");
  uint32_t loops = 0;
  k[LOOP] = gimg_doc_loop_count(doc, &loops) != 0;
  const size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    uint16_t num = 0, den = 0;
    gimg_item_frame_delay(gimg_doc_item(doc, i), &num, &den);
    if (num != 0) {
      k[DELAY] = true;
    }
    uint16_t hx = 0, hy = 0;
    gimg_item_hotspot(gimg_doc_item(doc, i), &hx, &hy);
    if (hx != 0 || hy != 0) {
      k[HOTSPOT] = true;
    }
  }
  // A role is only in question where there is a second item to hold it.
  k[ROLES] = n < 2 || gimg_item_role(gimg_doc_item(doc, 1)) == GIMG_ITEM_THUMBNAIL;
  k[ITEMS] = n >= 2;
  return k;
}

/** What the policy asked to have removed, as its documentation says. */
bool policy_removes(GIMG_Meta_Policy policy, const char * writer, int kind) {
  // The JPEG writer keeps the second item as the Exif thumbnail, in the APP1
  // the policies that drop metadata leave out.
  if (std::strcmp(writer, "jpeg") == 0 && (kind == ITEMS || kind == ROLES) &&
      (policy == GIMG_META_DROP_ALL || policy == GIMG_META_KEEP_COMMON_ONLY)) {
    return true;
  }
  const bool structural = kind == LOOP || kind == DELAY || kind == HOTSPOT ||
      kind == ROLES || kind == ITEMS;
  if (structural) {
    return false;
  }
  const bool raw_kind = kind == EXIF || kind == XMP || kind == TEXT ||
      kind == NATIVE || kind == COMMENTS;
  switch (policy) {
  case GIMG_META_DROP_ALL:
    return true;
  case GIMG_META_KEEP_COMMON_ONLY:
    return raw_kind;
  case GIMG_META_KEEP_RAW_ONLY:
    if (kind == RES || kind == DESC || kind == ICC) {
      return true;
    }
    return std::strcmp(writer, "png") == 0 &&
        (kind == EXIF || kind == XMP || kind == TEXT);
  case GIMG_META_STRIP_GPS:
    return kind == XMP;
  default:
    return false;
  }
}

std::string describe(const char * source, const char * writer, int policy) {
  return std::string(source) + " -> " + writer + " under " + policy_name[policy];
}

/**
 * The three properties for one save:
 *  - a warned kind is one the source held and the file lacks;
 *  - a kind the source held and the file lacks is warned about, unless the
 *    policy asked for it to go;
 *  - no kind is warned twice.
 */
void check_save(const std::string & what, const Set & held, const Saved & saved,
    GIMG_Meta_Policy policy, const char * writer, bool mixed = false) {
  ASSERT_EQ(saved.result, GIMG_OK) << what;
  const Set in_file = present_in(saved.bytes);
  Set warned = no_kinds();
  for (size_t i = 0; i < saved.warnings.size(); i++) {
    const int k = kind_of(saved.warnings[i]);
    ASSERT_GE(k, 0) << what << ": unrecognised warning: " << saved.warnings[i];
    EXPECT_FALSE(warned[k]) << what << ": " << kind_name[k] << " warned twice";
    warned[k] = true;
    EXPECT_STREQ(saved.warning_codecs[i].c_str(), writer) << what;
    EXPECT_TRUE(held[k]) << what << ": warned about " << kind_name[k]
                         << " the document does not hold";
    // A document that mixes blocks from several formats is warned of the
    // blocks the writer cannot read even while it writes its own.
    const bool raw_kind = k == EXIF || k == XMP || k == TEXT || k == NATIVE ||
        k == COMMENTS;
    if (!(mixed && raw_kind)) {
      EXPECT_FALSE(in_file[k])
          << what << ": warned about " << kind_name[k] << " that is in the file";
    }
  }
  for (int k = 0; k < KIND_COUNT; k++) {
    if (held[k] && !in_file[k] && !policy_removes(policy, writer, k)) {
      EXPECT_TRUE(warned[k]) << what << ": " << kind_name[k]
                             << " was lost without a warning";
    }
  }
}

// ---------------------------------------------------------------------------
// The sweep: every document through every writer under every policy
// ---------------------------------------------------------------------------

struct NamedSpec {
  const char * name;
  Spec spec;
};

std::vector<NamedSpec> all_specs() {
  std::vector<NamedSpec> v;
  Spec plain;
  v.push_back({"plain", plain});
  Spec s;
  s = Spec();
  s.jpeg = true;
  v.push_back({"jpeg-blocks", s});
  s = Spec();
  s.png = true;
  v.push_back({"png-exif", s});
  s = Spec();
  s.webp = true;
  v.push_back({"webp-blocks", s});
  s = Spec();
  s.tiff = true;
  v.push_back({"tiff-blocks", s});
  s = Spec();
  s.gif = true;
  v.push_back({"gif-comments", s});
  s = Spec();
  s.junk = true;
  v.push_back({"unknown-blocks", s});
  s = Spec();
  s.animated = true;
  v.push_back({"animated", s});
  s = Spec();
  s.jpeg = s.png = s.webp = s.tiff = s.gif = s.junk = s.animated = true;
  v.push_back({"everything", s});
  s = Spec();
  s.common = false;
  s.jpeg = true;
  v.push_back({"jpeg-blocks-no-common", s});
  return v;
}

TEST(MetaLossReport, EveryDocumentThroughEveryWriterUnderEveryPolicy) {
  for (const NamedSpec & ns : all_specs()) {
    GIMG_Doc * doc = make_doc(ns.spec);
    const Set held = holds(ns.spec);
    for (size_t w = 0; w < WRITER_COUNT; w++) {
      for (size_t p = 0; p < 6; p++) {
        SCOPED_TRACE(describe(ns.name, writers[w], static_cast<int>(p)));
        const Saved saved = save(doc, writers[w], policies[p], true);
        if (saved.result != GIMG_OK) {
          // A writer that refuses the document (an ICO of a cursor and a
          // frame) is not a case for the report.
          continue;
        }
        const bool mixed = std::strcmp(ns.name, "everything") == 0;
        check_save(describe(ns.name, writers[w], static_cast<int>(p)), held,
            saved, policies[p], writers[w], mixed);
      }
    }
    gimg_doc_destroy(doc);
  }
}

/** A document loaded from a file the writer @p from made of @p spec. */
TEST(MetaLossReport, LoadedDocumentsThroughEveryWriterUnderEveryPolicy) {
  Spec rich;
  rich.jpeg = rich.png = rich.webp = rich.tiff = rich.gif = true;
  rich.animated = true;
  for (size_t from = 0; from < WRITER_COUNT; from++) {
    GIMG_Doc * synthetic = make_doc(rich);
    const Saved origin =
        save(synthetic, writers[from], GIMG_META_PRESERVE_ALL, false);
    gimg_doc_destroy(synthetic);
    ASSERT_EQ(origin.result, GIMG_OK) << writers[from];
    Loaded loaded;
    ASSERT_EQ(loaded.load(origin.bytes), GIMG_OK) << writers[from];
    // What the loaded document holds is what its file held.
    const Set held = present_in(origin.bytes);
    for (size_t w = 0; w < WRITER_COUNT; w++) {
      for (size_t p = 0; p < 6; p++) {
        const std::string what =
            describe((std::string("loaded from ") + writers[from]).c_str(),
                writers[w], static_cast<int>(p));
        SCOPED_TRACE(what);
        const Saved saved = save(loaded.doc, writers[w], policies[p], true);
        if (saved.result != GIMG_OK) {
          continue;
        }
        check_save(what, held, saved, policies[p], writers[w]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// A PNG read from a file, whose text, XMP and private chunks live in its chunk
// list rather than in raw blocks
// ---------------------------------------------------------------------------

void put32(Bytes & b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> s));
  }
}

/** Insert chunk @p type with @p payload before the first IDAT of @p png. */
void insert_chunk(Bytes & png, const char * type, const Bytes & payload) {
  Bytes chunk;
  put32(chunk, static_cast<uint32_t>(payload.size()));
  chunk.insert(chunk.end(), type, type + 4);
  chunk.insert(chunk.end(), payload.begin(), payload.end());
  put32(chunk, exif_test::png_crc(chunk.data() + 4, chunk.size() - 4));
  size_t at = 8;
  while (at + 8 <= png.size() && std::memcmp(&png[at + 4], "IDAT", 4) != 0) {
    const uint32_t len = (static_cast<uint32_t>(png[at]) << 24) |
        (static_cast<uint32_t>(png[at + 1]) << 16) |
        (static_cast<uint32_t>(png[at + 2]) << 8) | png[at + 3];
    at += 12u + len;
  }
  png.insert(png.begin() + static_cast<long>(at), chunk.begin(), chunk.end());
}

Bytes png_with_chunks() {
  Spec plain;
  plain.common = false;
  GIMG_Doc * doc = make_doc(plain);
  Saved s = save(doc, "png", GIMG_META_DROP_ALL, false);
  gimg_doc_destroy(doc);
  Bytes png = s.bytes;
  insert_chunk(png, "tEXt", text_bytes(std::string("Author") + '\0' + MARK_TEXT));
  insert_chunk(png, "iTXt",
      text_bytes(std::string("XML:com.adobe.xmp") + '\0' + '\0' + '\0' + '\0' +
          '\0' + "<x:xmpmeta>" + MARK_XMP + "</x:xmpmeta>"));
  insert_chunk(png, "tIME", Bytes{0x07, 0xEA, 1, 2, 3, 4, 5});
  insert_chunk(png, "eXIf", exif_test::make_exif_with_gps());
  return png;
}

TEST(MetaLossReport, ALoadedPngsChunkListIsInventoried) {
  const Bytes png = png_with_chunks();
  Loaded loaded;
  ASSERT_EQ(loaded.load(png), GIMG_OK);
  Set held = no_kinds();
  held[EXIF] = held[XMP] = held[TEXT] = held[NATIVE] = true;
  for (size_t w = 0; w < WRITER_COUNT; w++) {
    for (size_t p = 0; p < 6; p++) {
      const std::string what =
          describe("loaded from a PNG with chunks", writers[w], static_cast<int>(p));
      SCOPED_TRACE(what);
      const Saved saved = save(loaded.doc, writers[w], policies[p], true);
      if (saved.result != GIMG_OK) {
        continue;
      }
      check_save(what, held, saved, policies[p], writers[w]);
    }
  }
  // The PNG writer, which does write them all, says nothing under the policy
  // that keeps them, and the others name each kind once.
  const Saved same = save(loaded.doc, "png", GIMG_META_PRESERVE_ALL, true);
  EXPECT_TRUE(same.warnings.empty());
  const Saved jpeg = save(loaded.doc, "jpeg", GIMG_META_PRESERVE_ALL, true);
  std::vector<std::string> expect = {
      "Exif dropped: the JPEG writer carries only Exif that arrived in JPEG form",
      "XMP dropped: the JPEG writer carries only XMP that arrived in JPEG form",
      "Text fields dropped: the JPEG writer does not write it",
      "Format-specific metadata dropped: the JPEG writer carries only "
      "format-specific metadata that arrived in JPEG form"};
  EXPECT_EQ(jpeg.warnings, expect);
}

// ---------------------------------------------------------------------------
// The exact warnings
// ---------------------------------------------------------------------------

TEST(MetaLossReport, ADocumentOfOnlyWhatTheFormatKeepsWarnsOfNothing) {
  Spec jpeg;
  jpeg.jpeg = true;
  GIMG_Doc * doc = make_doc(jpeg);
  EXPECT_TRUE(save(doc, "jpeg", GIMG_META_PRESERVE_ALL, true).warnings.empty());
  gimg_doc_destroy(doc);

  Spec tiff;
  tiff.tiff = true;
  doc = make_doc(tiff);
  EXPECT_TRUE(save(doc, "tiff", GIMG_META_PRESERVE_ALL, true).warnings.empty());
  gimg_doc_destroy(doc);

  Spec gif;
  gif.gif = true;
  doc = make_doc(gif);
  const Saved g = save(doc, "gif", GIMG_META_PRESERVE_ALL, true);
  // The profile and the resolution are all a GIF has no place for here.
  EXPECT_EQ(g.warnings,
      (std::vector<std::string>{
          "ICC profile dropped: the GIF format has no ICC profile",
          "Resolution dropped: the GIF format has no resolution"}));
  gimg_doc_destroy(doc);
}

TEST(MetaLossReport, WhatEveryWriterSaysOfADocumentThatCarriesEverything) {
  Spec all;
  all.jpeg = all.png = all.webp = all.tiff = all.gif = all.junk = true;
  all.animated = true;
  GIMG_Doc * doc = make_doc(all);
  const std::vector<std::vector<std::string>> expected = {
      // jpeg
      {
          "Exif dropped: the JPEG writer carries only Exif that arrived in JPEG form",
          "XMP dropped: the JPEG writer carries only XMP that arrived in JPEG form",
          "Additional comments dropped: the JPEG writer carries only comments that arrived in JPEG form",
          "Text fields dropped: the JPEG writer does not write it",
          "Format-specific metadata dropped: the JPEG writer carries only format-specific metadata that arrived in JPEG form",
          "Loop count dropped: the JPEG format has no animation loop count",
          "Frame delays dropped: the JPEG format has no frame delays",
          "Cursor hotspot dropped: the JPEG format has no cursor hotspot",
      },
      // png
      {
          "Exif dropped: the PNG writer carries only Exif that arrived in PNG form",
          "XMP dropped: the PNG writer carries only XMP that arrived in PNG form",
          "Additional comments dropped: the PNG writer does not write it",
          "Text fields dropped: the PNG writer carries only text fields that arrived in PNG form",
          "Format-specific metadata dropped: the PNG writer carries only format-specific metadata that arrived in PNG form",
          "Cursor hotspot dropped: the PNG format has no cursor hotspot",
          "Item roles dropped: the PNG writer writes every item as an animation frame",
      },
      // tiff
      {
          "Exif dropped: the TIFF writer does not write it",
          "XMP dropped: the TIFF writer carries only XMP that arrived in TIFF form",
          "Additional comments dropped: the TIFF writer does not write it",
          "Format-specific metadata dropped: the TIFF writer does not write it",
          "Loop count dropped: the TIFF format has no animation loop count",
          "Frame delays dropped: the TIFF format has no frame delays",
          "Cursor hotspot dropped: the TIFF format has no cursor hotspot",
          "Item roles dropped: the TIFF writer keeps the level role only; a thumbnail or alternate becomes a plain page",
      },
      // webp
      {
          "Exif dropped: the WebP writer carries only Exif that arrived in WebP form",
          "XMP dropped: the WebP writer carries only XMP that arrived in WebP form",
          "Resolution dropped: the WebP format has no resolution",
          "Description dropped: the WebP format has no description",
          "Additional comments dropped: the WebP format has no comments",
          "Text fields dropped: the WebP format has no text fields",
          "Format-specific metadata dropped: the WebP writer does not write it",
          "Cursor hotspot dropped: the WebP format has no cursor hotspot",
          "Item roles dropped: the WebP writer writes every item as an animation frame",
      },
      // gif
      {
          "Exif dropped: the GIF format has no Exif",
          "XMP dropped: the GIF format has no XMP",
          "ICC profile dropped: the GIF format has no ICC profile",
          "Resolution dropped: the GIF format has no resolution",
          "Additional comments dropped: the GIF writer carries only comments that arrived in GIF form",
          "Text fields dropped: the GIF writer does not write it",
          "Format-specific metadata dropped: the GIF writer does not write it",
          "Cursor hotspot dropped: the GIF format has no cursor hotspot",
          "Item roles dropped: the GIF writer writes every item as an animation frame",
      },
      // bmp
      {
          "Exif dropped: the BMP format has no Exif",
          "XMP dropped: the BMP format has no XMP",
          "Description dropped: the BMP format has no description",
          "Additional comments dropped: the BMP format has no comments",
          "Text fields dropped: the BMP format has no text fields",
          "Format-specific metadata dropped: the BMP format has no format-specific metadata",
          "Loop count dropped: the BMP format has no animation loop count",
          "Frame delays dropped: the BMP format has no frame delays",
          "Cursor hotspot dropped: the BMP format has no cursor hotspot",
          "Items dropped: the BMP writer writes the first item only",
      },
      // ico
      {
          "Exif dropped: the ICO format has no Exif",
          "XMP dropped: the ICO format has no XMP",
          "ICC profile dropped: the ICO format has no ICC profile",
          "Resolution dropped: the ICO format has no resolution",
          "Description dropped: the ICO format has no description",
          "Additional comments dropped: the ICO format has no comments",
          "Text fields dropped: the ICO format has no text fields",
          "Format-specific metadata dropped: the ICO format has no format-specific metadata",
          "Loop count dropped: the ICO format has no animation loop count",
          "Frame delays dropped: the ICO format has no frame delays",
          "Items dropped: the ICO writer writes only image and alternate items",
      },
  };
  const std::vector<std::vector<std::string>> expected_drop = {
      // jpeg
      {
          "Loop count dropped: the JPEG format has no animation loop count",
          "Frame delays dropped: the JPEG format has no frame delays",
          "Cursor hotspot dropped: the JPEG format has no cursor hotspot",
      },
      // png
      {
          "Cursor hotspot dropped: the PNG format has no cursor hotspot",
          "Item roles dropped: the PNG writer writes every item as an animation frame",
      },
      // tiff
      {
          "Loop count dropped: the TIFF format has no animation loop count",
          "Frame delays dropped: the TIFF format has no frame delays",
          "Cursor hotspot dropped: the TIFF format has no cursor hotspot",
          "Item roles dropped: the TIFF writer keeps the level role only; a thumbnail or alternate becomes a plain page",
      },
      // webp
      {
          "Cursor hotspot dropped: the WebP format has no cursor hotspot",
          "Item roles dropped: the WebP writer writes every item as an animation frame",
      },
      // gif
      {
          "Cursor hotspot dropped: the GIF format has no cursor hotspot",
          "Item roles dropped: the GIF writer writes every item as an animation frame",
      },
      // bmp
      {
          "Loop count dropped: the BMP format has no animation loop count",
          "Frame delays dropped: the BMP format has no frame delays",
          "Cursor hotspot dropped: the BMP format has no cursor hotspot",
          "Items dropped: the BMP writer writes the first item only",
      },
      // ico
      {
          "Loop count dropped: the ICO format has no animation loop count",
          "Frame delays dropped: the ICO format has no frame delays",
          "Items dropped: the ICO writer writes only image and alternate items",
      },
  };
  for (size_t w = 0; w < WRITER_COUNT; w++) {
    const Saved s = save(doc, writers[w], GIMG_META_PRESERVE_ALL, true);
    ASSERT_EQ(s.result, GIMG_OK) << writers[w];
    EXPECT_EQ(s.warnings, expected[w]) << writers[w];
    // Under GIMG_META_DROP_ALL the metadata kinds are the caller's to drop and
    // are not warned of; the playback and item kinds are not metadata.
    const Saved d = save(doc, writers[w], GIMG_META_DROP_ALL, true);
    ASSERT_EQ(d.result, GIMG_OK) << writers[w];
    EXPECT_EQ(d.warnings, expected_drop[w]) << writers[w];
  }
  gimg_doc_destroy(doc);
}

TEST(MetaLossReport, WhatThePolicyRemovedIsNotReportedAndWhatItKeptIs) {
  Spec plain;
  GIMG_Doc * doc = make_doc(plain);
  // Everything this document holds is metadata the policy was asked to drop.
  for (size_t w = 0; w < WRITER_COUNT; w++) {
    const Saved s = save(doc, writers[w], GIMG_META_DROP_ALL, true);
    ASSERT_EQ(s.result, GIMG_OK) << writers[w];
    EXPECT_TRUE(s.warnings.empty()) << writers[w];
  }
  // KEEP_COMMON_ONLY keeps the description, the resolution and the profile, so
  // a writer that leaves one out is a loss and not the policy's doing.
  EXPECT_EQ(save(doc, "jpeg", GIMG_META_KEEP_COMMON_ONLY, true).warnings,
      std::vector<std::string>{"Description dropped: the JPEG writer writes "
                               "no description under GIMG_META_KEEP_COMMON_ONLY"});
  EXPECT_EQ(save(doc, "png", GIMG_META_KEEP_COMMON_ONLY, true).warnings,
      std::vector<std::string>{"Description dropped: the PNG writer writes "
                               "no description under GIMG_META_KEEP_COMMON_ONLY"});
  EXPECT_EQ(save(doc, "webp", GIMG_META_KEEP_COMMON_ONLY, true).warnings,
      (std::vector<std::string>{
          "ICC profile dropped: the WebP writer writes no profile under "
          "GIMG_META_KEEP_COMMON_ONLY",
          "Resolution dropped: the WebP format has no resolution",
          "Description dropped: the WebP format has no description"}));
  EXPECT_TRUE(save(doc, "tiff", GIMG_META_KEEP_COMMON_ONLY, true).warnings.empty());
  EXPECT_TRUE(save(doc, "jpeg", GIMG_META_PRESERVE_ALL, true).warnings.empty());
  gimg_doc_destroy(doc);
}

TEST(MetaLossReport, WithoutADiagnosticsPointerNothingIsAskedFor) {
  Spec everything;
  everything.jpeg = everything.png = everything.gif = everything.animated = true;
  GIMG_Doc * doc = make_doc(everything);
  for (size_t w = 0; w < WRITER_COUNT; w++) {
    const Saved s = save(doc, writers[w], GIMG_META_PRESERVE_ALL, false);
    EXPECT_EQ(s.result, GIMG_OK) << writers[w];
    EXPECT_TRUE(s.warnings.empty()) << writers[w];
  }
  gimg_doc_destroy(doc);
}

// ---------------------------------------------------------------------------
// Nothing else changed
// ---------------------------------------------------------------------------

TEST(MetaLossReport, ReportingDoesNotChangeWhatIsWritten) {
  for (const NamedSpec & ns : all_specs()) {
    GIMG_Doc * doc = make_doc(ns.spec);
    for (size_t w = 0; w < WRITER_COUNT; w++) {
      for (size_t p = 0; p < 6; p++) {
        const Saved with = save(doc, writers[w], policies[p], true);
        const Saved without = save(doc, writers[w], policies[p], false);
        EXPECT_EQ(with.result, without.result)
            << describe(ns.name, writers[w], static_cast<int>(p));
        EXPECT_EQ(with.bytes, without.bytes)
            << describe(ns.name, writers[w], static_cast<int>(p));
        EXPECT_TRUE(without.warnings.empty());
      }
    }
    gimg_doc_destroy(doc);
  }
}

uint64_t fnv1a(const Bytes & b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) {
    h = (h ^ c) * 1099511628211ull;
  }
  return h;
}

/**
 * Bytes recorded before the report existed (FNV-1a 64 of the whole file) for the
 * document that carries everything, in policy order.  Only the writers whose
 * output is integer arithmetic over bytes this library alone produces are
 * listed: the PNG and GIF writers' output follows the compressor, and the JPEG
 * and WebP encoders' follows the platform's floating point.  Those are held to
 * the equality above instead.
 */
TEST(MetaLossReport, TheBytesAreTheOnesWrittenBeforeTheReportExisted) {
  struct Golden {
    const char * writer;
    uint64_t hash[6];
    size_t size[6];
  };
  const Golden golden[] = {
      {"tiff",
       {0x44ace808552f9cadull, 0xb036294128401f31ull, 0x04a4fa4f9b6b3b0eull, 0x44ace808552f9cadull, 0xc345963840d48a92ull, 0x6891f1988df60055ull},
       {1368, 1196, 1284, 1368, 1324, 1240}},
      {"bmp",
       {0xb80e2ee90118c2eeull, 0xf81501906c665b2bull, 0xb80e2ee90118c2eeull, 0xb80e2ee90118c2eeull, 0xf81501906c665b2bull, 0xb80e2ee90118c2eeull},
       {458, 246, 458, 458, 246, 458}},
      {"ico",
       {0x278809ba489236aeull, 0x278809ba489236aeull, 0x278809ba489236aeull, 0x278809ba489236aeull, 0x278809ba489236aeull, 0x278809ba489236aeull},
       {350, 350, 350, 350, 350, 350}},
  };
  Spec all;
  all.jpeg = all.png = all.webp = all.tiff = all.gif = all.junk = true;
  all.animated = true;
  GIMG_Doc * doc = make_doc(all);
  for (const Golden & g : golden) {
    for (size_t p = 0; p < 6; p++) {
      const Saved s = save(doc, g.writer, policies[p], true);
      ASSERT_EQ(s.result, GIMG_OK) << g.writer;
      EXPECT_EQ(s.bytes.size(), g.size[p]) << g.writer << " " << policy_name[p];
      EXPECT_EQ(fnv1a(s.bytes), g.hash[p]) << g.writer << " " << policy_name[p];
    }
  }
  gimg_doc_destroy(doc);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
