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
 * along with Ghoti.io Image.  If not, see <https://www.gnu.org/licenses/>.
 */
/**
 * @file
 *
 * Every file this library writes is one it can read.
 *
 * The four verify_<fmt>_output.py scripts read our output back with an
 * outside decoder and compare pixels. Nothing read it back with *our* decoder,
 * and our decoder is the strictest reader available - it rejects a corrupted
 * CRC, a duplicate IHDR and a missing IEND, all three of which Pillow accepts.
 *
 * That gap let a real bug through. The APNG writer numbered its fcTL and fdAT
 * chunks from the frame index rather than from a running counter, so every
 * animation with a frame too big for one fdAT came out with the numbering
 * wrong. Pillow and ImageMagick read those files without complaint, the pixels
 * were right, and every gate stayed green; this library's own loader was the
 * only thing that objected, and nothing was asking it.
 *
 * So this walks every fixture, saves it as every format that will take it, and
 * loads the result back. A save this codec refuses is not a failure - GIF
 * cannot hold more than 256 colours and says so. Writing something and then
 * refusing to read it is.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

namespace {

/** Every format this library can write. */
const char * const kFormats[] = {"png", "jpeg", "bmp", "gif"};

/** Those of them that can hold more than one frame. */
bool holds_frames(const char * fmt) {
  return std::strcmp(fmt, "png") == 0 || std::strcmp(fmt, "gif") == 0;
}

std::vector<std::string> fixtures() {
  std::vector<std::string> out;
  for (const char * dir : {GIMG_TEST_DATA_PNG, GIMG_TEST_DATA_JPEG,
           GIMG_TEST_DATA_BMP, GIMG_TEST_DATA_GIF}) {
    std::error_code ec;
    for (const auto & e : std::filesystem::directory_iterator(dir, ec)) {
      if (!e.is_regular_file()) {
        continue;
      }
      const std::string ext = e.path().extension().string();
      if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
          ext == ".gif") {
        out.push_back(e.path().string());
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<uint8_t> slurp(const std::string & path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

/**
 * A raster of incompressible noise.
 *
 * Every fixture in the tree is small, and small is the one thing this class of
 * bug hides behind: a writer that splits its output at 32 KiB has a second,
 * rarely-taken path that no tidy 4x4 test image ever reaches. The APNG
 * numbering bug lived exactly there, and the fixture-driven half of this file
 * does not catch it - that was measured, not assumed.
 */
GIMG_Raster * noise_raster(uint32_t w, uint32_t h, uint32_t seed) {
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

/** How many chunks of `type` the largest run in this PNG has. */
size_t longest_run_of(const std::vector<uint8_t> & png, const char * type) {
  size_t i = 8, run = 0, best = 0;
  while (i + 8 <= png.size()) {
    const uint32_t len = (static_cast<uint32_t>(png[i]) << 24) |
        (static_cast<uint32_t>(png[i + 1]) << 16) |
        (static_cast<uint32_t>(png[i + 2]) << 8) |
        static_cast<uint32_t>(png[i + 3]);
    const bool match = std::memcmp(&png[i + 4], type, 4) == 0;
    run = match ? run + 1 : 0;
    if (run > best) {
      best = run;
    }
    if (std::memcmp(&png[i + 4], "IEND", 4) == 0) {
      break;
    }
    i += 12u + static_cast<size_t>(len);
  }
  return best;
}

/** True when the JPEG carries an APP1 segment beginning "Exif\0\0". */
bool has_exif_app1(const std::vector<uint8_t> & jpg) {
  size_t i = 2;
  while (i + 4 <= jpg.size() && jpg[i] == 0xFF) {
    const uint8_t m = jpg[i + 1];
    if (m == 0xD9 || m == 0xDA) {
      break;
    }
    const size_t ln = (static_cast<size_t>(jpg[i + 2]) << 8) | jpg[i + 3];
    if (m == 0xE1 && i + 4 + 6 <= jpg.size() &&
        std::memcmp(&jpg[i + 4], "Exif\0\0", 6) == 0) {
      return true;
    }
    i += 2 + ln;
  }
  return false;
}

std::vector<uint8_t> save_as_jpeg(GIMG_Doc * doc) {
  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    return {};
  }
  GIMG_Save_Options opts;
  memset(&opts, 0, sizeof(opts));
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  std::vector<uint8_t> bytes;
  if (gimg_doc_save(doc, out, "jpeg", &opts, &report) == GIMG_OK) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out, &p, &n);
    bytes.assign(static_cast<const uint8_t *>(p),
        static_cast<const uint8_t *>(p) + n);
  }
  gimg_stream_destroy(out);
  return bytes;
}

/** Decode `doc`'s items in one of the orders a caller might plausibly use. */
void decode_in_order(GIMG_Doc * doc, int order) {
  const size_t n = gimg_doc_item_count(doc);
  switch (order) {
  case 0:
    break; // nothing decoded; the writer decodes what it needs
  case 1:
    if (n) {
      gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr);
    }
    break;
  case 2:
    for (size_t i = 0; i < n; i++) {
      gimg_item_ensure_decoded(gimg_doc_item(doc, i), nullptr);
    }
    break;
  case 3:
    for (size_t i = n; i-- > 0;) {
      gimg_item_ensure_decoded(gimg_doc_item(doc, i), nullptr);
    }
    break;
  default: // everything, then the first again: the canvas cache is monotone
    for (size_t i = 0; i < n; i++) {
      gimg_item_ensure_decoded(gimg_doc_item(doc, i), nullptr);
    }
    if (n) {
      gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr);
    }
    break;
  }
}

TEST(CrossCodec, WhatTheCallerDecodedFirstDoesNotChangeTheFile) {
  // What a writer produces should be a function of the document and the
  // options, and of nothing else. It was not: the JPEG writer took item 1 as
  // an EXIF thumbnail whenever that item happened to carry a decoded raster,
  // so walking an animation's frames before saving it changed the file.
  //
  // The general form is worth holding still, because the specific one was
  // found by accident. Every writer takes an attached raster when there is
  // one and decodes when there is not; any of them branching on *which*
  // happened is this bug again.
  //
  // Only multi-item documents are swept here. The mechanism needs an item to
  // be in one state or the other while another item is read, and a single
  // image has nothing to disagree with - a one-off sweep over all 256
  // fixtures in four output formats and six decode orders (5142 comparisons)
  // found the same nothing, at five seconds a run.
  static const char * const formats[] = {"png", "jpeg", "bmp", "gif"};
  size_t compared = 0;
  for (const std::string & path : fixtures()) {
    const std::vector<uint8_t> src = slurp(path);
    if (src.size() < 16) {
      continue;
    }
    {
      GIMG_Stream * probe = nullptr;
      if (gimg_stream_create_memory(src.data(), src.size(), &probe) !=
          GIMG_OK) {
        continue;
      }
      GIMG_Doc * d = nullptr;
      const bool ok = gimg_doc_load(probe, nullptr, nullptr, &d) == GIMG_OK;
      const size_t items = ok ? gimg_doc_item_count(d) : 0;
      if (d) {
        gimg_doc_destroy(d);
      }
      gimg_stream_destroy(probe);
      if (items < 2) {
        continue;
      }
    }
    for (const char * fmt : formats) {
      std::vector<uint8_t> reference;
      int reference_order = -1;
      for (int order = 0; order < 5; order++) {
        GIMG_Stream * s = nullptr;
        ASSERT_EQ(gimg_stream_create_memory(src.data(), src.size(), &s),
            GIMG_OK);
        GIMG_Doc * doc = nullptr;
        if (gimg_doc_load(s, nullptr, nullptr, &doc) != GIMG_OK) {
          gimg_stream_destroy(s);
          continue;
        }
        decode_in_order(doc, order);
        GIMG_Stream * out = nullptr;
        ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
        GIMG_Save_Options opts;
        memset(&opts, 0, sizeof(opts));
        opts.gif_alpha_threshold = 128;
        GIMG_Save_Report report;
        memset(&report, 0, sizeof(report));
        std::vector<uint8_t> bytes;
        if (gimg_doc_save(doc, out, fmt, &opts, &report) == GIMG_OK) {
          const void * p = nullptr;
          size_t n = 0;
          gimg_stream_output_buffer(out, &p, &n);
          bytes.assign(static_cast<const uint8_t *>(p),
              static_cast<const uint8_t *>(p) + n);
        }
        gimg_stream_destroy(out);
        gimg_doc_destroy(doc);
        gimg_stream_destroy(s);
        if (bytes.empty()) {
          continue; // refused, and refused the same way every time
        }
        if (reference_order < 0) {
          reference = bytes;
          reference_order = order;
          continue;
        }
        compared++;
        EXPECT_EQ(bytes, reference)
            << fmt << " from " << path << ": decode order " << order
            << " produced " << bytes.size() << " bytes where order "
            << reference_order << " produced " << reference.size();
      }
    }
  }
  EXPECT_GT(compared, 50u)
      << "too few multi-item fixtures compared to mean anything";
}

TEST(CrossCodec, AnAnimationFrameIsNotEmbeddedAsAnExifThumbnail) {
  // A JPEG carries its thumbnail as item 1, so the writer encodes item 1 into
  // IFD1. In an animation item 1 is frame two, and saying it is a thumbnail is
  // a claim the source never made.
  //
  // The writer knew that and guarded it with the loading codec - but the guard
  // only covered *decoding* item 1, and an already-decoded raster was taken
  // from any document at all. So whether a caller had walked the frames before
  // saving decided what came out: on a 49-frame 1200x1200 GIF, 87 KB with a
  // 51,905-byte APP1 against 35 KB without, from one document and one set of
  // options. On eight larger animations the segment passed the 65533 bytes an
  // APP1 can hold and the save failed outright with GIMG_ERR_LIMIT.
  //
  // The second assertion is the one that matters most: what the caller
  // happened to decode must not change the file.
  const std::string fixture =
      std::string(GIMG_TEST_DATA_GIF) + "/gif_12x8_background_index.gif";
  const std::vector<uint8_t> src = slurp(fixture);
  ASSERT_FALSE(src.empty()) << "Run tests/data/gif/generate.py";

  std::vector<uint8_t> saved[2];
  for (int decode_all = 0; decode_all < 2; decode_all++) {
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(src.data(), src.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    const size_t items = gimg_doc_item_count(doc);
    ASSERT_GE(items, 2u) << "the fixture must have a second frame to misuse";
    const size_t upto = decode_all ? items : 1u;
    for (size_t i = 0; i < upto; i++) {
      ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, i), nullptr),
          GIMG_OK);
    }
    saved[decode_all] = save_as_jpeg(doc);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
    ASSERT_FALSE(saved[decode_all].empty())
        << "saving an animation as JPEG was refused (decode_all="
        << decode_all << ")";
    EXPECT_FALSE(has_exif_app1(saved[decode_all]))
        << "frame two was written into an EXIF thumbnail (decode_all="
        << decode_all << ")";
  }
  EXPECT_EQ(saved[0], saved[1])
      << "the same document and options produced different files depending on "
         "which frames the caller had decoded first";
}

TEST(SelfRoundTrip, AnAnimationTooBigForOneChunkSurvivesToo) {
  // Three frames of noise, each far past the 32 KiB the writer splits at, so
  // the multi-chunk path is taken for every frame.
  std::vector<GIMG_Raster *> frames;
  for (uint32_t i = 0; i < 3; i++) {
    GIMG_Raster * raster = noise_raster(128, 128, i + 1u);
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

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts;
  memset(&opts, 0, sizeof(opts));
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  ASSERT_EQ(gimg_doc_save(doc, out, "png", &opts, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  const std::vector<uint8_t> bytes(static_cast<const uint8_t *>(p),
      static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  // Leave it where verify_structure.py looks, and do it *before* the checks
  // below. That script reads the bytes against the specification rather than
  // decoding them, and it can only examine what the tests emit - every other
  // APNG in that directory comes from a fixture small enough to fit one
  // chunk. Publishing after an assertion would mean the file appears only
  // when everything already passed, which is when it is least needed.
  {
    const std::string path =
        std::string(GIMG_TEST_OUT_PNG) + "/apng_split_frames.png";
    std::ofstream f(path, std::ios::binary);
    EXPECT_TRUE(f) << "cannot write " << path;
    if (f) {
      f.write(reinterpret_cast<const char *>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
    }
  }

  // Without this the test is vacuous: frames that fit one chunk take the
  // ordinary path, and this whole case exists for the other one.
  ASSERT_GT(longest_run_of(bytes, "fdAT"), 1u)
      << "the noise frames did not split across chunks, so this is not "
         "exercising the multi-chunk path";

  GIMG_Stream * back = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(bytes.data(), bytes.size(), &back), GIMG_OK);
  GIMG_Doc * reloaded = nullptr;
  ASSERT_EQ(gimg_doc_load(back, nullptr, nullptr, &reloaded), GIMG_OK)
      << "wrote an APNG whose frames span several chunks and then refused to "
         "read it back";
  EXPECT_EQ(gimg_doc_item_count(reloaded), 3u);
  gimg_doc_destroy(reloaded);
  gimg_stream_destroy(back);

}

TEST(SelfRoundTrip, EveryFileWeWriteIsOneWeCanRead) {
  const std::vector<std::string> files = fixtures();
  // A fixture directory that has moved would make this test pass by checking
  // nothing, which is the failure mode it exists to prevent elsewhere.
  ASSERT_GT(files.size(), 100u)
      << "found only " << files.size() << " fixtures; the data directories "
      << "are probably not where this was built to look";

  size_t sources = 0, written = 0, refused = 0;
  for (const std::string & path : files) {
    const std::vector<uint8_t> src = slurp(path);
    if (src.size() < 16) {
      continue;
    }
    GIMG_Stream * s = nullptr;
    if (gimg_stream_create_memory(src.data(), src.size(), &s) != GIMG_OK) {
      continue;
    }
    GIMG_Doc * doc = nullptr;
    if (gimg_doc_load(s, nullptr, nullptr, &doc) != GIMG_OK) {
      gimg_stream_destroy(s); // Many fixtures are malformed on purpose.
      continue;
    }
    const size_t items = gimg_doc_item_count(doc);
    bool decoded = items > 0;
    for (size_t i = 0; i < items; i++) {
      if (gimg_item_ensure_decoded(gimg_doc_item(doc, i), nullptr) != GIMG_OK) {
        decoded = false;
      }
    }
    if (!decoded) {
      gimg_doc_destroy(doc);
      gimg_stream_destroy(s);
      continue;
    }
    sources++;

    for (const char * fmt : kFormats) {
      GIMG_Stream * out = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
      GIMG_Save_Options opts;
      memset(&opts, 0, sizeof(opts));
      // Without a threshold a partly transparent pixel is refused rather than
      // decided about, which would hide most of the corpus from this test.
      opts.gif_alpha_threshold = 128;
      GIMG_Save_Report report;
      memset(&report, 0, sizeof(report));
      const GIMG_Result sr = gimg_doc_save(doc, out, fmt, &opts, &report);
      if (sr != GIMG_OK) {
        refused++;
        gimg_stream_destroy(out);
        continue; // A codec that cannot hold this says so; that is an answer.
      }
      const void * p = nullptr;
      size_t n = 0;
      gimg_stream_output_buffer(out, &p, &n);
      const std::vector<uint8_t> bytes(static_cast<const uint8_t *>(p),
          static_cast<const uint8_t *>(p) + n);
      gimg_stream_destroy(out);
      written++;

      GIMG_Stream * back = nullptr;
      ASSERT_EQ(
          gimg_stream_create_memory(bytes.data(), bytes.size(), &back),
          GIMG_OK);
      GIMG_Doc * reloaded = nullptr;
      const GIMG_Result lr =
          gimg_doc_load(back, nullptr, nullptr, &reloaded);
      EXPECT_EQ(lr, GIMG_OK)
          << "wrote " << fmt << " from " << path << " and then refused to "
          << "read it back";
      if (lr == GIMG_OK) {
        const size_t got = gimg_doc_item_count(reloaded);
        if (holds_frames(fmt)) {
          EXPECT_EQ(got, items)
              << fmt << " from " << path << " lost frames on the way back";
        }
        for (size_t i = 0; i < got; i++) {
          GIMG_Raster * raster = nullptr;
          EXPECT_EQ(gimg_item_decode(gimg_doc_item(reloaded, i), nullptr,
                        &raster),
              GIMG_OK)
              << fmt << " from " << path << ": frame " << i
              << " will not decode";
          if (raster) {
            gimg_raster_destroy(raster);
          }
        }
        gimg_doc_destroy(reloaded);
      }
      gimg_stream_destroy(back);
    }
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }

  EXPECT_GT(sources, 100u) << "too few fixtures loaded and decoded to mean "
                              "anything";
  EXPECT_GT(written, 400u) << "too few files written to mean anything";
  std::printf(
      "    %zu sources, %zu files written and read back, %zu saves refused\n",
      sources, written, refused);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
