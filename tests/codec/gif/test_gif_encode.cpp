/**
 * @file
 *
 * GIF encode tests.
 *
 * Each file the encoder writes is left in GIMG_TEST_OUT_GIF with one sidecar
 * per frame holding the pixels that frame was meant to show, for
 * tests/data/gif/verify_gif_output.py to read back with giflib and Pillow.
 * This codec agreeing with itself proves nothing about either half.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "gif_test_utils.h"
#include <fstream>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using gif_test::Loaded;
using gif_test::Rgba;

namespace {

/** Build an RGBA8 raster from a function of x and y. */
GIMG_Raster * make_raster(
    uint32_t w, uint32_t h, Rgba (*pixel)(uint32_t, uint32_t)) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &raster) != GIMG_OK) {
    return nullptr;
  }
  auto * base = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      const Rgba p = pixel(x, y);
      uint8_t * px = base + y * stride + x * 4u;
      px[0] = p.r;
      px[1] = p.g;
      px[2] = p.b;
      px[3] = p.a;
    }
  }
  return raster;
}

/** Sixteen colours in a pattern with runs, so LZW has something to chew. */
Rgba sixteen(uint32_t x, uint32_t y) {
  const uint8_t i = static_cast<uint8_t>(((x / 2u) + y) % 16u);
  return Rgba{static_cast<uint8_t>(i * 17u), static_cast<uint8_t>(255u - i * 9u),
      static_cast<uint8_t>(i * 5u + 3u), 255};
}

/** A hole of fully transparent pixels in an otherwise opaque field. */
Rgba with_hole(uint32_t x, uint32_t y) {
  if (x >= 2u && x < 5u && y >= 1u && y < 3u) {
    return Rgba{0, 0, 0, 0};
  }
  return Rgba{static_cast<uint8_t>(40u + x * 20u),
      static_cast<uint8_t>(90u + y * 30u), 200u, 255};
}

/** Every one of the 256 colours a GIF can hold, and no more. */
Rgba all_256(uint32_t x, uint32_t y) {
  const uint8_t i = static_cast<uint8_t>((y * 16u + x) & 0xFFu);
  return Rgba{i, static_cast<uint8_t>(255u - i), static_cast<uint8_t>(i ^ 0x5Au),
      255};
}

/**
 * Save one raster as a GIF, returning the bytes.
 *
 * Takes ownership: gimg_item_set_raster() hands the raster to the item, and
 * destroying the document destroys it.  Callers must not free it themselves.
 */
GIMG_Result save_raster(GIMG_Raster * raster, const GIMG_Save_Options * options,
    std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK) {
    return GIMG_ERR_OOM;
  }
  GIMG_Item * item = gimg_doc_item(doc, 0);
  gimg_item_set_raster(item, raster);
  GIMG_Stream * stream = nullptr;
  if (gimg_stream_create_memory_output(&stream) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return GIMG_ERR_OOM;
  }
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  const GIMG_Result r = gimg_doc_save(doc, stream, "gif", options, &report);
  if (r == GIMG_OK) {
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &data, &size);
    out.assign(static_cast<const uint8_t *>(data),
        static_cast<const uint8_t *>(data) + size);
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r;
}

/**
 * Save several rasters as one animation.  Takes ownership of each.
 *
 * `prepare`, when given, is called with the document after the rasters are
 * attached and before it is saved - the hook the metadata tests need to set a
 * description on the very document that is about to be written.
 */
GIMG_Result save_frames(const std::vector<GIMG_Raster *> & rasters,
    const GIMG_Save_Options * options, std::vector<uint8_t> & out,
    const std::function<void(GIMG_Doc *)> & prepare = nullptr) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK) {
    return GIMG_ERR_OOM;
  }
  if (gimg_doc_set_item_count(doc, rasters.size()) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return GIMG_ERR_OOM;
  }
  for (size_t i = 0; i < rasters.size(); i++) {
    gimg_item_set_raster(gimg_doc_item(doc, i), rasters[i]);
    gimg_item_set_frame_delay(gimg_doc_item(doc, i), 5, 100);
  }
  if (prepare) {
    prepare(doc);
  }
  GIMG_Stream * stream = nullptr;
  if (gimg_stream_create_memory_output(&stream) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return GIMG_ERR_OOM;
  }
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  const GIMG_Result r = gimg_doc_save(doc, stream, "gif", options, &report);
  if (r == GIMG_OK) {
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &data, &size);
    out.assign(static_cast<const uint8_t *>(data),
        static_cast<const uint8_t *>(data) + size);
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r;
}

/** Leave a file and its per-frame expectations for the verifier. */
void publish(const char * name, const std::vector<uint8_t> & bytes,
    const std::vector<std::vector<uint8_t>> & frames_rgba,
    const char * comment = nullptr) {
  const std::string path = std::string(GIMG_TEST_OUT_GIF) + "/" + name;
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out) << "cannot write " << path;
  out.write(reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  out.close();
  for (size_t i = 0; i < frames_rgba.size(); i++) {
    std::ofstream side(
        path + ".expected." + std::to_string(i) + ".rgba", std::ios::binary);
    ASSERT_TRUE(side) << "cannot write the expectation beside " << path;
    side.write(reinterpret_cast<const char *>(frames_rgba[i].data()),
        static_cast<std::streamsize>(frames_rgba[i].size()));
  }
  if (comment) {
    // Read back by outside decoders the same way the pixels are; see
    // verify_gif_output.py for why it checks containment rather than equality.
    std::ofstream side(path + ".expected.comment", std::ios::binary);
    ASSERT_TRUE(side) << "cannot write the comment expectation beside " << path;
    side << comment;
  }
}

/** The RGBA bytes a function of x and y produces, for a sidecar. */
std::vector<uint8_t> expectation(
    uint32_t w, uint32_t h, Rgba (*pixel)(uint32_t, uint32_t)) {
  std::vector<uint8_t> out;
  out.reserve(static_cast<size_t>(w) * h * 4u);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      const Rgba p = pixel(x, y);
      out.push_back(p.r);
      out.push_back(p.g);
      out.push_back(p.b);
      out.push_back(p.a);
    }
  }
  return out;
}

uint16_t read_u16(const std::vector<uint8_t> & b, size_t at) {
  return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

} // namespace

TEST(GifEncode, WritesAHeaderAndScreenDescriptor) {
  GIMG_Raster * raster = make_raster(16, 8, sixteen);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, nullptr, bytes), GIMG_OK);

  ASSERT_GT(bytes.size(), 13u);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 6), "GIF89a");
  EXPECT_EQ(read_u16(bytes, 6), 16u);
  EXPECT_EQ(read_u16(bytes, 8), 8u);
  EXPECT_EQ(bytes.back(), 0x3Bu) << "trailer";
  publish("opaque_16x8.gif", bytes, {expectation(16, 8, sixteen)});
}

TEST(GifEncode, RoundTripsThroughOurOwnDecoder) {
  GIMG_Raster * raster = make_raster(16, 8, sixteen);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, nullptr, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 16u);
  ASSERT_EQ(img.height(), 8u);
  for (uint32_t y = 0; y < 8u; y++) {
    for (uint32_t x = 0; x < 16u; x++) {
      ASSERT_EQ(img.at(x, y), sixteen(x, y)) << "at " << x << "," << y;
    }
  }
}

TEST(GifEncode, AFullyTransparentPixelBecomesTheTransparentIndex) {
  GIMG_Raster * raster = make_raster(8, 4, with_hole);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, nullptr, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.at(3, 1).a, 0u) << "inside the hole";
  EXPECT_EQ(img.at(0, 0).a, 255u) << "outside it";
  EXPECT_EQ(img.at(0, 0), with_hole(0, 0));
  publish("transparent_8x4.gif", bytes, {expectation(8, 4, with_hole)});
}

TEST(GifEncode, AllTwoHundredAndFiftySixColoursFit) {
  // The largest palette the format has.  Counting past index 255 to say the
  // table holds 256 is where an eight-bit counter wraps, and the file that
  // results names a one-entry palette.
  GIMG_Raster * raster = make_raster(16, 16, all_256);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, nullptr, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t y = 0; y < 16u; y++) {
    for (uint32_t x = 0; x < 16u; x++) {
      ASSERT_EQ(img.at(x, y), all_256(x, y)) << "at " << x << "," << y;
    }
  }
  publish("palette_256_16x16.gif", bytes, {expectation(16, 16, all_256)});
}

TEST(GifEncode, MoreThanTwoHundredAndFiftySixColoursIsRefused) {
  // Refused rather than quantized: choosing which colours to keep is an
  // image-processing decision, and the PNG writer here draws the same line.
  GIMG_Raster * raster = make_raster(64, 8, [](uint32_t x, uint32_t y) {
    const uint32_t n = y * 64u + x;  // 512 distinct colours
    return Rgba{static_cast<uint8_t>(n & 0xFFu),
        static_cast<uint8_t>((n >> 1) & 0xFFu),
        static_cast<uint8_t>(n >> 8), 255};
  });
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  EXPECT_EQ(save_raster(raster, nullptr, bytes), GIMG_ERR_UNSUPPORTED);
}

TEST(GifEncode, PartialAlphaIsRefusedUnlessAThresholdIsGiven) {
  // GIF has one bit of transparency.  An alpha of 128 cannot be stored, only
  // decided about, and the caller owns that decision.
  auto half = [](uint32_t x, uint32_t) {
    return Rgba{200, 100, 50, static_cast<uint8_t>(x == 0u ? 128u : 255u)};
  };
  // A raster per attempt: save_raster() hands ownership to the document it
  // builds, so the one the refused call consumed is gone by the time the
  // second call runs.
  std::vector<uint8_t> bytes;
  GIMG_Raster * refused = make_raster(4, 2, half);
  ASSERT_NE(refused, nullptr);
  EXPECT_EQ(save_raster(refused, nullptr, bytes), GIMG_ERR_UNSUPPORTED)
      << "no threshold: the codec must not choose";

  GIMG_Raster * raster = make_raster(4, 2, half);
  ASSERT_NE(raster, nullptr);
  GIMG_Save_Options options;
  memset(&options, 0, sizeof(options));
  options.gif_alpha_threshold = 128;
  ASSERT_EQ(save_raster(raster, &options, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  // 128 is at the threshold, so it rounds to opaque.
  EXPECT_EQ(img.at(0, 0).a, 255u);
}

TEST(GifEncode, InterlacedOutputHoldsTheSamePixels) {
  GIMG_Raster * raster = make_raster(16, 8, sixteen);
  ASSERT_NE(raster, nullptr);
  GIMG_Save_Options options;
  memset(&options, 0, sizeof(options));
  options.gif_interlace = 1;
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, &options, bytes), GIMG_OK);

  // The interlace bit must actually be set, or this test passes by writing a
  // progressive file and reading it back correctly.
  ASSERT_GT(bytes.size(), 23u);
  EXPECT_TRUE((bytes[22] & 0x40u) != 0u) << "image descriptor interlace flag";

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t y = 0; y < 8u; y++) {
    for (uint32_t x = 0; x < 16u; x++) {
      ASSERT_EQ(img.at(x, y), sixteen(x, y)) << "at " << x << "," << y;
    }
  }
  publish("interlaced_16x8.gif", bytes, {expectation(16, 8, sixteen)});
}

namespace {

/** Frame 0: entirely opaque. */
Rgba anim_first(uint32_t x, uint32_t y) {
  return Rgba{static_cast<uint8_t>(20u + x * 30u),
      static_cast<uint8_t>(60u + y * 40u), 180u, 255};
}

/** Frame 1: transparent exactly where frame 0 was opaque and coloured. */
Rgba anim_second(uint32_t x, uint32_t y) {
  if (y == 0u) {
    return Rgba{0, 0, 0, 0};
  }
  return Rgba{220u, static_cast<uint8_t>(10u + x * 25u), 30u, 255};
}

} // namespace

TEST(GifEncode, ALaterFrameIsTransparentWhereAnEarlierOneWasOpaque) {
  // The frames this encoder writes are whole canvases that have already been
  // composited, so each must start from an empty screen.  Written with
  // disposal left unspecified they instead draw on top of one another, and a
  // pixel meant to be see-through shows the frame before it.  Frame 0 looks
  // right either way, which is what made this worth a test: the error appears
  // only from frame 1 onwards, and only where transparency meets an earlier
  // opaque pixel.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(6, 3, anim_first));
  frames.push_back(make_raster(6, 3, anim_second));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);

  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(img.doc()), 2u);

  ASSERT_EQ(img.decode(nullptr, 0), GIMG_OK);
  EXPECT_EQ(img.at(0, 0), anim_first(0, 0));

  ASSERT_EQ(img.decode(nullptr, 1), GIMG_OK);
  for (uint32_t x = 0; x < 6u; x++) {
    EXPECT_EQ(img.at(x, 0).a, 0u)
        << "frame 1 row 0 must be transparent, not frame 0 showing through, at x="
        << x;
  }
  EXPECT_EQ(img.at(0, 1), anim_second(0, 1));

  publish("animation_6x3.gif", bytes,
      {expectation(6, 3, anim_first), expectation(6, 3, anim_second)});
}

TEST(GifEncode, AnAnimationCarriesItsDelayAndLoopCount) {
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(4, 2, sixteen));
  frames.push_back(make_raster(4, 2, sixteen));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);

  GIMG_Save_Options options;
  memset(&options, 0, sizeof(options));
  options.gif_loop_count = 7;
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, &options, bytes), GIMG_OK);

  // The NETSCAPE2.0 Application Extension follows the screen descriptor.
  const std::string all(bytes.begin(), bytes.end());
  EXPECT_NE(all.find("NETSCAPE2.0"), std::string::npos)
      << "an animation must say how often it repeats";

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(img.doc()), 2u);
  uint16_t num = 0, den = 0;
  gimg_item_frame_delay(gimg_doc_item(img.doc(), 0), &num, &den);
  EXPECT_EQ(num, 5u) << "hundredths of a second, as written";
  EXPECT_EQ(den, 100u);

  // The count written is the count read back, which is what makes the round
  // trip below mean anything.
  uint32_t loops = 0;
  EXPECT_EQ(gimg_doc_loop_count(img.doc(), &loops), 1);
  EXPECT_EQ(loops, 7u);
}

TEST(GifEncode, ALoopCountSurvivesALoadAndSaveWhenTheCallerCarriesIt) {
  // The documented round trip, run rather than asserted.  Save does not reach
  // into the document for the count on its own: gif_loop_count's 0 already
  // means forever, so it has no spelling for "unset" and cannot fall back
  // without changing what an existing caller's 0 means.  Two lines of caller
  // code close it, and this is those two lines.
  Loaded source;
  ASSERT_EQ(source.load("gif_4x2_netscape_loop.gif"), GIMG_OK);
  uint32_t loops = 0;
  ASSERT_EQ(gimg_doc_loop_count(source.doc(), &loops), 1);
  ASSERT_EQ(loops, 5u);

  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(4, 2, sixteen));
  frames.push_back(make_raster(4, 2, sixteen));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);

  GIMG_Save_Options options;
  memset(&options, 0, sizeof(options));
  options.gif_loop_count = static_cast<uint16_t>(loops);  // the two lines
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, &options, bytes), GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint32_t round_tripped = 0;
  EXPECT_EQ(gimg_doc_loop_count(again.doc(), &round_tripped), 1);
  EXPECT_EQ(round_tripped, 5u);
}

// ---------------------------------------------------------------------------
// Comments, written
// ---------------------------------------------------------------------------
//
// Create, update and delete, through the public metadata API.  Each case is
// checked by reading the bytes back, so what is asserted is what a file
// carries and not what the writer intended.

namespace {

/** Count the Comment Extensions in a GIF's bytes, and collect their text. */
std::vector<std::string> comments_in(const std::vector<uint8_t> & bytes) {
  std::vector<std::string> out;
  for (size_t i = 0; i + 2u < bytes.size(); i++) {
    if (bytes[i] != 0x21u || bytes[i + 1] != 0xFEu) {
      continue;
    }
    // A sub-block chain: a length byte, that many bytes, until a zero length.
    std::string text;
    size_t at = i + 2u;
    while (at < bytes.size() && bytes[at] != 0u) {
      const size_t len = bytes[at];
      if (at + 1u + len > bytes.size()) {
        break;
      }
      text.append(reinterpret_cast<const char *>(&bytes[at + 1u]), len);
      at += 1u + len;
    }
    out.push_back(text);
  }
  return out;
}

/** Save one small raster with the given options. */
GIMG_Result save_with(const GIMG_Save_Options * options,
    std::vector<uint8_t> & bytes,
    const std::function<void(GIMG_Doc *)> & prepare) {
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(4, 2, sixteen));
  if (!frames[0]) {
    return GIMG_ERR_OOM;
  }
  return save_frames(frames, options, bytes, prepare);
}

} // namespace

TEST(GifComments, ACommentIsPublishedForOutsideDecodersToRead) {
  // Written to tests/out/gif/ with a sidecar naming the text, so that
  // verify_gif_output.py asks Pillow and ImageMagick whether they can read it.
  // Our own reader agreeing with our own writer would prove nothing.
  static const char * const kText = "ghoti.io image test comment";
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(16, 8, sixteen));
  ASSERT_NE(frames[0], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes,
                [](GIMG_Doc * doc) {
                  GIMG_Meta_Common * common = nullptr;
                  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &common), GIMG_OK);
                  ASSERT_EQ(gimg_meta_common_set_description(common, kText),
                      GIMG_OK);
                }),
      GIMG_OK);
  publish("commented_16x8.gif", bytes, {expectation(16, 8, sixteen)}, kText);
}

TEST(GifComments, ADescriptionIsWrittenAsACommentExtension) {
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_with(nullptr, bytes,
                [](GIMG_Doc * doc) {
                  GIMG_Meta_Common * common = nullptr;
                  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &common), GIMG_OK);
                  ASSERT_EQ(
                      gimg_meta_common_set_description(common, "written here"),
                      GIMG_OK);
                }),
      GIMG_OK);
  const std::vector<std::string> found = comments_in(bytes);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0], "written here");
}

TEST(GifComments, NoDescriptionMeansNoCommentExtension) {
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_with(nullptr, bytes, [](GIMG_Doc *) {}), GIMG_OK);
  EXPECT_TRUE(comments_in(bytes).empty())
      << "a file with nothing to say must not carry an empty comment";
}

TEST(GifComments, DropAllRemovesIt) {
  GIMG_Save_Options options;
  memset(&options, 0, sizeof(options));
  options.metadata_policy = GIMG_META_DROP_ALL;
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_with(&options, bytes,
                [](GIMG_Doc * doc) {
                  GIMG_Meta_Common * common = nullptr;
                  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &common), GIMG_OK);
                  ASSERT_EQ(gimg_meta_common_set_description(common, "gone"),
                      GIMG_OK);
                }),
      GIMG_OK);
  EXPECT_TRUE(comments_in(bytes).empty());
}

TEST(GifComments, ALongCommentIsChainedAcrossSubBlocks) {
  // A sub-block holds 255 bytes at most (89a 15), so anything longer has to be
  // split - and read back as one string, which is what says the chain was
  // written correctly rather than truncated at the first block.
  const std::string various(700u, 'x');
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_with(nullptr, bytes,
                [&various](GIMG_Doc * doc) {
                  GIMG_Meta_Common * common = nullptr;
                  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &common), GIMG_OK);
                  ASSERT_EQ(gimg_meta_common_set_description(
                                common, various.c_str()),
                      GIMG_OK);
                }),
      GIMG_OK);
  const std::vector<std::string> found = comments_in(bytes);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].size(), 700u);
  EXPECT_EQ(found[0], various);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
