/**
 * @file
 *
 * GIF decode tests.
 *
 * The fixtures come from tests/data/gif/generate.py; the ones assembled byte
 * by byte there carry an LZW encoder written out in that file, so a fixture
 * does not depend on the code it checks.  Every expectation below was taken
 * from giflib rather than from this decoder - see documentation/formats/gif.md
 * for where each oracle's reach ends.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "gif_test_utils.h"
#include <atomic>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using gif_test::Loaded;
using gif_test::Rgba;

namespace {

// The palette generate.py writes, in order.  Distinct in all three channels so
// a channel swap cannot read as a match.
const Rgba kRed{255, 0, 0, 255};
const Rgba kGreen{0, 255, 0, 255};
const Rgba kBlue{0, 0, 255, 255};
const Rgba kYellow{255, 255, 0, 255};
const Rgba kTransparent{0, 0, 0, 0};

} // namespace

TEST(GifDecode, PlainFileDecodesToItsPalette) {
  Loaded img;
  ASSERT_EQ(img.load("gif_16x8_plain.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.width(), 16u);
  EXPECT_EQ(img.height(), 8u);
  EXPECT_EQ(img.at(0, 0), kRed);
  EXPECT_EQ(img.at(1, 0), kGreen);
}

TEST(GifDecode, InterlacedMatchesProgressive) {
  // The same picture written both ways.  Comparing them against each other
  // catches a wrong pass table without needing either one's pixels spelled
  // out, and fails if the four-pass order is applied to a progressive file.
  Loaded plain;
  ASSERT_EQ(plain.load("gif_16x8_plain.gif"), GIMG_OK);
  ASSERT_EQ(plain.decode(), GIMG_OK);
  Loaded woven;
  ASSERT_EQ(woven.load("gif_16x8_interlaced.gif"), GIMG_OK);
  ASSERT_EQ(woven.decode(), GIMG_OK);

  ASSERT_EQ(plain.width(), woven.width());
  ASSERT_EQ(plain.height(), woven.height());
  for (uint32_t y = 0; y < plain.height(); y++) {
    for (uint32_t x = 0; x < plain.width(); x++) {
      ASSERT_EQ(plain.at(x, y), woven.at(x, y)) << "at " << x << "," << y;
    }
  }
}

TEST(GifDecode, FileWithNoGlobalColorTableUsesTheLocalOne) {
  // 89a 18 makes the Global Color Table optional.  A decoder that assumes one
  // is always present reads this through the wrong table or refuses it.
  Loaded img;
  ASSERT_EQ(img.load("gif_8x4_no_global_table.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.width(), 8u);
  EXPECT_EQ(img.height(), 4u);
  EXPECT_EQ(img.at(0, 0), kRed);
  EXPECT_EQ(img.at(1, 0), kGreen);
  EXPECT_EQ(img.at(2, 0), kBlue);
  EXPECT_EQ(img.at(3, 0), kYellow);
}

TEST(GifDecode, NarrowCodeWidthsDecode) {
  // This file's minimum code size is 2, not 8.  Every width below 8 used to
  // fail in the compression library underneath, which is most GIFs: of 121 on
  // one Debian machine, 71 held an image narrower than 8.
  Loaded img;
  ASSERT_EQ(img.load("gif_8x4_no_global_table.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  // A full canvas, not a prefix: the failure being guarded against returned
  // success after writing a fraction of the pixels.
  bool any_unwritten = false;
  for (uint32_t y = 0; y < img.height(); y++) {
    for (uint32_t x = 0; x < img.width(); x++) {
      if (img.at(x, y).a == 0) {
        any_unwritten = true;
      }
    }
  }
  EXPECT_FALSE(any_unwritten) << "every pixel of this file is opaque";
}

TEST(GifDecode, IndexPastTheEndOfThePaletteDrawsNothing) {
  // The code size is set independently of the table size, so a four-entry
  // table can be addressed with index 7.  giflib leaves such a pixel alone;
  // Pillow paints it opaque black.  This codec follows giflib, and refusing
  // the file - the third option - would reject images both of them display.
  Loaded img;
  ASSERT_EQ(img.load("gif_8x2_index_past_palette.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.at(0, 0), kRed);
  EXPECT_EQ(img.at(4, 0), kTransparent) << "index 7 of a four-entry table";
  EXPECT_EQ(img.at(7, 0), kTransparent);
}

TEST(GifDecode, ExtensionsAreWalkedPastRatherThanParsed) {
  // Comment and Plain Text blocks before and after the image.  Nothing renders
  // either; what matters is that the sub-block chain is followed so the image
  // after them is still found.
  Loaded img;
  ASSERT_EQ(img.load("gif_6x3_extensions.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.width(), 6u);
  EXPECT_EQ(img.height(), 3u);
}

TEST(GifDecode, Gif87aReadsTheSameAs89a) {
  Loaded img;
  ASSERT_EQ(img.load("gif_4x4_87a.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.width(), 4u);
  EXPECT_EQ(img.height(), 4u);
  EXPECT_EQ(img.at(0, 0), kRed);
}

TEST(GifDecode, FrameSmallerThanTheCanvasLandsAtItsOffset) {
  // The canvas is 12x8; the second image is 4x4 at (6,2).  A decoder that
  // ignores the position draws it at the origin, and one that ignores the
  // logical screen returns a 4x4 raster for the second frame.
  Loaded img;
  ASSERT_EQ(img.load("gif_12x8_offset_frame.gif"), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(img.doc()), 2u);

  ASSERT_EQ(img.decode(nullptr, 1), GIMG_OK);
  EXPECT_EQ(img.width(), 12u) << "the canvas, not the patch";
  EXPECT_EQ(img.height(), 8u);
  // The patch's corner is index 7; its centre is index 3.
  EXPECT_EQ(img.at(7, 3), kYellow);
  // Outside the patch, the first frame still shows through.
  EXPECT_NE(img.at(0, 0), kTransparent);
}

TEST(GifDecode, DisposalPreviousRestoresWhatWasThereBefore) {
  // Three frames: a background, a patch that asks to be undone, then a second
  // patch.  The last frame must composite over the first, not the second.
  // Disposal 3 is the one writers use least and decoders get wrong most.
  Loaded img;
  ASSERT_EQ(img.load("gif_8x8_disposal_previous.gif"), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(img.doc()), 3u);
  ASSERT_EQ(img.decode(nullptr, 2), GIMG_OK);

  // (0,0) was covered by the middle frame, which was then undone, so the first
  // frame's colour must be back.
  EXPECT_EQ(img.at(0, 0), kRed) << "middle frame was not undone";
  // The last frame's own pixels are on top.
  EXPECT_EQ(img.at(2, 2), kBlue);
}

TEST(GifDecode, TransparentPixelsLeaveThePreviousFrameShowing) {
  Loaded img;
  ASSERT_EQ(img.load("gif_6x4_transparent_over_previous.gif"), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(img.doc()), 2u);
  ASSERT_EQ(img.decode(nullptr, 1), GIMG_OK);

  // The second frame writes index 0 - its transparent index - where (x+y) is
  // odd, and index 5 where it is even.  So the first frame's green shows
  // through the odd squares and is covered on the even ones.
  EXPECT_NE(img.at(0, 0), kGreen) << "index 5: frame 1 covers";
  EXPECT_EQ(img.at(1, 0), kGreen) << "index 0 is transparent: frame 0 shows";
}

TEST(GifDecode, FrameDelayAndDisposalReachTheItem) {
  Loaded img;
  ASSERT_EQ(img.load("gif_12x8_offset_frame.gif"), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(img.doc(), 0);
  ASSERT_NE(item, nullptr);
  uint16_t num = 0, den = 0;
  gimg_item_frame_delay(item, &num, &den);
  // GIF counts hundredths of a second (89a 23); the item model carries a
  // fraction, so the denominator has to say which unit the numerator is in.
  EXPECT_EQ(num, 10u);
  EXPECT_EQ(den, 100u);
  EXPECT_EQ(gimg_item_dispose_op(item), GIMG_DISPOSE_NONE);
}

TEST(GifDecode, TruncatedFileIsRefusedRatherThanGuessedAt) {
  // A file that stops inside its code stream.  It is refused at load, with
  // GIMG_ERR_FORMAT, because that is what gimg_stream_read_exact() returns for
  // a short read throughout this library - not a decision this codec makes.
  Loaded img;
  const GIMG_Result r = img.load("gif_16x16_truncated.gif");
  ASSERT_NE(r, GIMG_OK) << "a truncated file must not load as a whole one";
  EXPECT_EQ(r, GIMG_ERR_FORMAT);
}

TEST(GifDecode, NetscapeLoopCountIsRead) {
  // Not exposed through the public item model yet; what this pins is that the
  // Application Extension is walked correctly and the image after it is found.
  Loaded img;
  ASSERT_EQ(img.load("gif_4x2_netscape_loop.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.width(), 4u);
  EXPECT_EQ(img.at(0, 0), kRed);
}

// ---------------------------------------------------------------------------
// The canvas cache
// ---------------------------------------------------------------------------
//
// Decoding frame N composites frames 0 through N, and the decoder keeps the
// canvas it arrived at so the next frame does not have to build it again.
// That is an optimization, which means it has exactly one thing to prove: the
// answer must not depend on it.  Every test below fixes some order of decodes
// against the answer a decoder with no cache at all would give - a freshly
// loaded document per frame - rather than against pixels written out here, so
// that they keep checking the invariant even if the fixture changes.
//
// The fixture is the one built for this: seven frames using every disposal
// method, over overlapping patches, with transparency under each of them.

namespace {

/** Every pixel of a decoded frame, so two decodes can be compared whole. */
std::vector<uint8_t> frame_pixels(const Loaded & img) {
  std::vector<uint8_t> out;
  for (uint32_t y = 0; y < img.height(); y++) {
    for (uint32_t x = 0; x < img.width(); x++) {
      const Rgba p = img.at(x, y);
      out.insert(out.end(), {p.r, p.g, p.b, p.a});
    }
  }
  return out;
}

/** Decode one frame of a document loaded for that frame alone: no cache. */
std::vector<uint8_t> decode_cold(const char * fixture, size_t index) {
  Loaded fresh;
  EXPECT_EQ(fresh.load(fixture), GIMG_OK);
  EXPECT_EQ(fresh.decode(nullptr, index), GIMG_OK);
  return frame_pixels(fresh);
}

const char * const kCycle = "gif_10x6_disposal_cycle.gif";

} // namespace

TEST(GifCanvasCache, ForwardWalkMatchesAColdDecodeOfEachFrame) {
  // The order a player uses, and the only one the cache is ever warm for.  A
  // cache that stored the canvas before disposal, or applied a disposal twice,
  // shows up here from the second frame on.
  Loaded img;
  ASSERT_EQ(img.load(kCycle), GIMG_OK);
  const size_t count = gimg_doc_item_count(img.doc());
  ASSERT_EQ(count, 7u);
  for (size_t i = 0; i < count; i++) {
    ASSERT_EQ(img.decode(nullptr, i), GIMG_OK) << "frame " << i;
    EXPECT_EQ(frame_pixels(img), decode_cold(kCycle, i)) << "frame " << i;
  }
}

TEST(GifCanvasCache, ReverseWalkMatchesAColdDecodeOfEachFrame) {
  // Backwards the cache is always ahead of what is asked for, so every frame
  // replays from the beginning.  What this pins is that being unusable is all
  // that happens - that a cache for a later frame is not mistaken for one that
  // can be built on.
  Loaded img;
  ASSERT_EQ(img.load(kCycle), GIMG_OK);
  const size_t count = gimg_doc_item_count(img.doc());
  for (size_t i = count; i-- > 0;) {
    ASSERT_EQ(img.decode(nullptr, i), GIMG_OK) << "frame " << i;
    EXPECT_EQ(frame_pixels(img), decode_cold(kCycle, i)) << "frame " << i;
  }
}

TEST(GifCanvasCache, JumpingAboutMatchesAColdDecodeOfEachFrame) {
  // Neither walk: the seeks a scrubbing UI makes.  Each one lands on a cache
  // left by some unrelated frame, which is the case neither walk above covers.
  Loaded img;
  ASSERT_EQ(img.load(kCycle), GIMG_OK);
  for (size_t i : {size_t(3), size_t(1), size_t(6), size_t(6), size_t(0),
           size_t(4), size_t(2), size_t(5)}) {
    ASSERT_EQ(img.decode(nullptr, i), GIMG_OK) << "frame " << i;
    EXPECT_EQ(frame_pixels(img), decode_cold(kCycle, i)) << "frame " << i;
  }
}

TEST(GifCanvasCache, DecodingOneFrameTwiceGivesTheSamePixels) {
  // The cache is written after the frame it was asked for, so decoding that
  // same frame again reads back a cache for a frame after it.  Off by one in
  // either direction and the second answer differs from the first.
  Loaded img;
  ASSERT_EQ(img.load(kCycle), GIMG_OK);
  ASSERT_EQ(img.decode(nullptr, 4), GIMG_OK);
  const std::vector<uint8_t> first = frame_pixels(img);
  ASSERT_EQ(img.decode(nullptr, 4), GIMG_OK);
  EXPECT_EQ(frame_pixels(img), first);
}

TEST(GifCanvasCache, ASingleImageGifStillDecodes) {
  // Nothing caches a one-frame file - there is no later frame to hand a head
  // start to - so this is the path where cache_state is null throughout.
  Loaded img;
  ASSERT_EQ(img.load("gif_16x8_plain.gif"), GIMG_OK);
  ASSERT_EQ(img.decode(nullptr, 0), GIMG_OK);
  EXPECT_EQ(frame_pixels(img), decode_cold("gif_16x8_plain.gif", 0));
}

TEST(GifCanvasCache, ThreadsSharingOneDocumentAgreeWithAColdDecode) {
  // The document is const to decode, and the cache is the one thing decode
  // writes through it.  Four threads walking the same document at once is what
  // says the lock around that write is doing its job: without it they tear
  // each other's canvas, and under ASan they read one that has been freed.
  //
  // The expected pixels are computed first, on this thread, so that a thread
  // is comparing against an answer no thread produced.
  Loaded img;
  ASSERT_EQ(img.load(kCycle), GIMG_OK);
  const size_t count = gimg_doc_item_count(img.doc());
  std::vector<std::vector<uint8_t>> expected;
  for (size_t i = 0; i < count; i++) {
    expected.push_back(decode_cold(kCycle, i));
  }

  std::atomic<int> mismatches{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&, t]() {
      // Each thread walks in a different direction and starts somewhere else,
      // so they are contending rather than politely taking turns.
      for (int pass = 0; pass < 8; pass++) {
        for (size_t k = 0; k < count; k++) {
          const size_t i =
              (t % 2) ? count - 1 - k : (k + (size_t)t) % count;
          GIMG_Item * item = gimg_doc_item(img.doc(), i);
          GIMG_Raster * raster = nullptr;
          if (gimg_item_decode(item, nullptr, &raster) != GIMG_OK) {
            failures++;
            continue;
          }
          std::vector<uint8_t> got;
          const uint8_t * px = static_cast<const uint8_t *>(
              gimg_raster_pixels_const(raster));
          const size_t stride = gimg_raster_stride_bytes(raster);
          const uint32_t w = gimg_raster_width(raster);
          const uint32_t h = gimg_raster_height(raster);
          for (uint32_t y = 0; y < h; y++) {
            got.insert(got.end(), px + y * stride, px + y * stride + w * 4u);
          }
          if (got != expected[i]) {
            mismatches++;
          }
          gimg_raster_destroy(raster);
        }
      }
    });
  }
  for (std::thread & th : threads) {
    th.join();
  }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(mismatches.load(), 0);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
