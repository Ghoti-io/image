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
    std::vector<uint8_t> & out,
    const std::function<void(GIMG_Doc *)> & prepare = nullptr) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK) {
    return GIMG_ERR_OOM;
  }
  GIMG_Item * item = gimg_doc_item(doc, 0);
  gimg_item_set_raster(item, raster);
  if (prepare) {
    // The hook the document-level tests need: the raster is attached and the
    // document is about to be written, which is the only moment a property of
    // *this* document can be set.
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

/** A plain background, the same in every frame of the patch test. */
Rgba patch_base(uint32_t x, uint32_t y) {
  return Rgba{static_cast<uint8_t>(30u + (x % 5u) * 20u),
      static_cast<uint8_t>(40u + (y % 4u) * 25u), 90u, 255};
}

/** The same picture with one 3x2 block recoloured, at (10,6). */
Rgba patch_changed(uint32_t x, uint32_t y) {
  if (x >= 10u && x < 13u && y >= 6u && y < 8u) {
    return Rgba{250u, 12u, 200u, 255};
  }
  return patch_base(x, y);
}

/** Two changes at opposite corners, so the rectangle between them is
 * unchanged and has to be masked rather than repainted. */
Rgba patch_two_corners(uint32_t x, uint32_t y) {
  if ((x == 1u && y == 1u) || (x == 22u && y == 14u)) {
    return Rgba{250u, 12u, 200u, 255};
  }
  return patch_base(x, y);
}

/** A colour that is unique per index n, for building large palettes. */
Rgba indexed_colour(uint32_t n) {
  return Rgba{static_cast<uint8_t>(n & 0xFFu),
      static_cast<uint8_t>((n >> 8) & 0xFFu), 77u, 255};
}

/** Frame 0 of the local-palette pair: two hundred colours. */
Rgba many_colours_first(uint32_t x, uint32_t y) {
  return indexed_colour((y * 32u + x) % 200u);
}

/** Frame 1: one pixel in five takes a colour the first frame never used, so
 * the two frames together need more than a global table holds - and four in
 * five are unchanged and have to be masked. */
Rgba many_colours_second(uint32_t x, uint32_t y) {
  if ((x + y) % 5u == 0u) {
    return indexed_colour(200u + ((y * 32u + x) % 102u));
  }
  return many_colours_first(x, y);
}

/** Every image block in a GIF, as position and size, plus its control block. */
struct Block {
  uint32_t x, y, w, h;
  /** Whether the image descriptor carried a Local Color Table. */
  bool local_table;
  /** The Graphic Control Extension packed field, or 0 when there was none. */
  uint8_t gce;
  bool transparent() const {
    return (gce & 0x01u) != 0u;
  }
  uint8_t disposal() const {
    return static_cast<uint8_t>((gce >> 2) & 0x07u);
  }
};

/** Entries in the Global Color Table, or 0 when the file has none. */
uint32_t global_table_entries(const std::vector<uint8_t> & bytes) {
  if (bytes.size() < 13u || !(bytes[10] & 0x80u)) {
    return 0u;
  }
  return 1u << ((bytes[10] & 7u) + 1u);
}

std::vector<Block> image_blocks(const std::vector<uint8_t> & bytes) {
  std::vector<Block> out;
  size_t i = 13u;
  if (bytes.size() > 10u && (bytes[10] & 0x80u)) {
    i += size_t(3) << ((bytes[10] & 7u) + 1u);
  }
  auto u16 = [&bytes](size_t at) {
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8);
  };
  uint8_t pending_gce = 0u;
  while (i < bytes.size()) {
    if (bytes[i] == 0x3Bu) {
      break;
    }
    if (bytes[i] == 0x21u) {
      if (bytes[i + 1u] == 0xF9u && i + 3u < bytes.size()) {
        pending_gce = bytes[i + 3u];
      }
      i += 2u;
      while (i < bytes.size() && bytes[i] != 0u) {
        i += 1u + bytes[i];
      }
      i += 1u;
    }
    else if (bytes[i] == 0x2Cu) {
      const uint8_t descriptor = bytes[i + 9u];
      out.push_back(Block{u16(i + 1u), u16(i + 3u), u16(i + 5u), u16(i + 7u),
          (descriptor & 0x80u) != 0u, pending_gce});
      pending_gce = 0u;
      const uint8_t packed = descriptor;
      i += 10u;
      if (packed & 0x80u) {
        i += size_t(3) << ((packed & 7u) + 1u);
      }
      i += 1u;  // LZW minimum code size
      while (i < bytes.size() && bytes[i] != 0u) {
        i += 1u + bytes[i];
      }
      i += 1u;
    }
    else {
      break;
    }
  }
  return out;
}

} // namespace

TEST(GifEncode, AFrameIsWrittenAsOnlyTheRectangleThatChanged) {
  // The caller hands this encoder whole composited canvases; a GIF frame is a
  // patch.  Writing every frame at full size is correct and was what this did,
  // but it means an animation where one small block moves costs a full canvas
  // per frame.  Here one 3x2 block at (10,6) changes and nothing else does.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(24, 16, patch_base));
  frames.push_back(make_raster(24, 16, patch_changed));
  frames.push_back(make_raster(24, 16, patch_base));
  for (GIMG_Raster * f : frames) {
    ASSERT_NE(f, nullptr);
  }
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);

  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 3u);
  // The first frame has nothing before it to patch, so it is the whole screen.
  EXPECT_EQ(blocks[0].w, 24u);
  EXPECT_EQ(blocks[0].h, 16u);
  // The others are the changed block and no more.
  for (size_t i = 1; i < 3u; i++) {
    EXPECT_EQ(blocks[i].x, 10u) << "frame " << i;
    EXPECT_EQ(blocks[i].y, 6u) << "frame " << i;
    EXPECT_EQ(blocks[i].w, 3u) << "frame " << i;
    EXPECT_EQ(blocks[i].h, 2u) << "frame " << i;
  }

  // Smaller is only worth anything if it still says the same thing.  The
  // sidecars go to outside decoders through verify_gif_output.py.
  publish("patched_24x16.gif", bytes,
      {expectation(24, 16, patch_base), expectation(24, 16, patch_changed),
          expectation(24, 16, patch_base)});
}

TEST(GifEncode, AnUnchangedFrameCostsOnePixel) {
  // A GIF image block cannot be zero-sized, so a frame identical to the one
  // before it is written as one pixel repainting its own colour.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(24, 16, patch_base));
  frames.push_back(make_raster(24, 16, patch_base));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);
  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_EQ(blocks[1].w, 1u);
  EXPECT_EQ(blocks[1].h, 1u);
}

namespace {

/** A field whose corners alone change between frames. */
Rgba corners_base(uint32_t x, uint32_t y) {
  return Rgba{static_cast<uint8_t>(20u + (x % 7u) * 15u),
      static_cast<uint8_t>(30u + (y % 6u) * 18u), 120u, 255};
}

Rgba corners_changed(uint32_t x, uint32_t y) {
  const bool corner = (x == 1u && y == 1u) || (x == 22u && y == 14u);
  return corner ? Rgba{255u, 0u, 255u, 255u} : corners_base(x, y);
}

} // namespace

TEST(GifEncode, UnchangedPixelsInsideAFrameAreWrittenAsTransparent) {
  // Cropping cannot help when the two pixels that changed are at opposite
  // corners: the rectangle has to span almost the whole canvas.  What makes
  // that cheap is that everything inside it which did *not* change is written
  // as the transparent index, so the screen shows through and the code stream
  // becomes a run of one index.
  //
  // The signature is that the second frame declares a transparent index even
  // though the raster handed to the encoder has no transparent pixel in it.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(24, 16, corners_base));
  frames.push_back(make_raster(24, 16, corners_changed));
  for (GIMG_Raster * f : frames) {
    ASSERT_NE(f, nullptr);
  }
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);

  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_EQ(blocks[1].x, 1u);
  EXPECT_EQ(blocks[1].y, 1u);
  EXPECT_EQ(blocks[1].w, 22u);
  EXPECT_EQ(blocks[1].h, 14u);
  EXPECT_TRUE(blocks[1].transparent())
      << "the frame spans the canvas and is opaque; without masking there is "
         "nothing for a transparent index to be for";
  // Leaving the screen showing through is only correct if the screen holds
  // what it should, so the frames go to outside decoders to composite.
  publish("masked_24x16.gif", bytes,
      {expectation(24, 16, corners_base),
          expectation(24, 16, corners_changed)});
}

TEST(GifEncode, FramesThatShareAPaletteShareOneTable) {
  // 89a 18 lets one table serve every frame, and this writer used to decline
  // it.  For a short animation of few colours that was most of the file: a
  // twelve-frame spinner of sixteen colours spent 576 of its 1964 bytes on
  // twelve copies of the same table.
  std::vector<GIMG_Raster *> frames;
  for (int i = 0; i < 4; i++) {
    frames.push_back(make_raster(24, 16,
        i % 2 == 0 ? patch_base : patch_changed));
  }
  for (GIMG_Raster * f : frames) {
    ASSERT_NE(f, nullptr);
  }
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);

  EXPECT_GT(global_table_entries(bytes), 0u) << "no Global Color Table";
  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 4u);
  for (size_t i = 0; i < blocks.size(); i++) {
    EXPECT_FALSE(blocks[i].local_table)
        << "frame " << i << " carries a table of its own as well";
  }
  publish("global_table_24x16.gif", bytes,
      {expectation(24, 16, patch_base), expectation(24, 16, patch_changed),
          expectation(24, 16, patch_base),
          expectation(24, 16, patch_changed)});
}

TEST(GifEncode, ASingleFrameGetsNoGlobalTable) {
  // One global table and one local table are the same size, so there is
  // nothing to win and the walk that would find out is skipped.
  std::vector<uint8_t> bytes;
  GIMG_Raster * raster = make_raster(16, 8, sixteen);
  ASSERT_NE(raster, nullptr);
  ASSERT_EQ(save_raster(raster, nullptr, bytes), GIMG_OK);
  EXPECT_EQ(global_table_entries(bytes), 0u);
  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 1u);
  EXPECT_TRUE(blocks[0].local_table);
}

TEST(GifEncode, FramesThatDoNotShareAPaletteKeepTheirOwnTables) {
  // Between them these two use more colours than one table can hold, so there
  // is no global table to write and each frame carries its own - which is
  // what this encoder did for every animation before.
  auto half_of_256 = [](uint32_t base) {
    return [base](uint32_t x, uint32_t y) {
      const uint32_t i = base + (y * 16u + x);
      return Rgba{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 1),
          static_cast<uint8_t>(i ^ 0x5Au), 255};
    };
  };
  std::vector<GIMG_Raster *> frames;
  for (uint32_t base : {0u, 256u}) {
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                  nullptr, 0, &r),
        GIMG_OK);
    auto * p = static_cast<uint8_t *>(gimg_raster_pixels(r));
    const size_t stride = gimg_raster_stride_bytes(r);
    const auto fn = half_of_256(base);
    for (uint32_t y = 0; y < 16u; y++) {
      for (uint32_t x = 0; x < 16u; x++) {
        const Rgba c = fn(x, y);
        uint8_t * px = p + y * stride + x * 4u;
        px[0] = c.r;
        px[1] = c.g;
        px[2] = c.b;
        px[3] = c.a;
      }
    }
    frames.push_back(r);
  }
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);
  EXPECT_EQ(global_table_entries(bytes), 0u);
  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 2u);
  for (const Block & b : blocks) {
    EXPECT_TRUE(b.local_table);
  }
}

TEST(GifEncode, SavingALoadedAnimationSurvivesAWarmCanvasCache) {
  // Saving walks the frames twice - once to see whether they share a palette,
  // once to write them - and the decoder's canvas cache only ever moves
  // forward.  A caller who has already walked the document leaves that cache
  // at the last frame, and the writer's own first walk leaves it there again.
  //
  // Getting that wrong is a performance fault rather than a wrong answer (110
  // seconds instead of 4.3 on a 358-frame animation, before the writer began
  // clearing the cache between its two passes), which is exactly the kind that
  // no assertion about pixels would catch.  What this pins is the half that
  // can be asserted: that the output is right whatever state the cache was
  // left in.  The seven frames of the fixture use every disposal method.
  Loaded source;
  ASSERT_EQ(source.load("gif_10x6_disposal_cycle.gif"), GIMG_OK);
  const size_t frames = gimg_doc_item_count(source.doc());
  ASSERT_GT(frames, 2u);

  // Walk it forward first, so the cache is parked at the end before saving.
  std::vector<std::vector<Rgba>> expected;
  for (size_t i = 0; i < frames; i++) {
    ASSERT_EQ(source.decode(nullptr, i), GIMG_OK) << "frame " << i;
    std::vector<Rgba> frame;
    for (uint32_t y = 0; y < source.height(); y++) {
      for (uint32_t x = 0; x < source.width(); x++) {
        frame.push_back(source.at(x, y));
      }
    }
    expected.push_back(frame);
  }

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  ASSERT_EQ(gimg_doc_save(source.doc(), out, "gif", nullptr, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),
      static_cast<const uint8_t *>(data) + size);
  gimg_stream_destroy(out);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(again.doc()), frames);
  for (size_t i = 0; i < frames; i++) {
    ASSERT_EQ(again.decode(nullptr, i), GIMG_OK) << "frame " << i;
    size_t at = 0;
    for (uint32_t y = 0; y < again.height(); y++) {
      for (uint32_t x = 0; x < again.width(); x++, at++) {
        const Rgba got = again.at(x, y);
        const Rgba want = expected[i][at];
        // A pixel both sides call invisible may differ in the colour beneath.
        if (got.a == 0u && want.a == 0u) {
          continue;
        }
        EXPECT_EQ(got, want)
            << "frame " << i << " at " << x << "," << y;
      }
    }
  }
}

TEST(GifEncode, MaskingGivesUpRatherThanRefusingAFullPalette) {
  // Masking spends a palette entry on the transparent index.  A frame of
  // exactly 256 colours has none to spare - and such a frame was writable
  // before masking existed, so it must still be.  The encoder falls back to
  // writing every pixel its own colour.
  //
  // Reaching this needs a frame that both fills the table with pixels that
  // changed and has at least one that did not, because masking usually
  // *reduces* the colour count: a masked pixel contributes no colour.
  const uint32_t w = 17u, h = 16u; // 272 pixels: 256 colours and a tail
  auto colour_at = [](size_t i) {
    const size_t c = i < 256u ? i : 0u;
    return Rgba{static_cast<uint8_t>(c), static_cast<uint8_t>(255u - c),
        static_cast<uint8_t>(c ^ 0x5Au), 255};
  };
  // Built by hand, because the pattern depends on the pixel's index rather
  // than on its coordinates, which is all make_raster's callback is given.
  auto build = [&](bool all_colours) {
    GIMG_Raster * r = nullptr;
    if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr,
            0, &r) != GIMG_OK) {
      return static_cast<GIMG_Raster *>(nullptr);
    }
    auto * base = static_cast<uint8_t *>(gimg_raster_pixels(r));
    const size_t stride = gimg_raster_stride_bytes(r);
    for (uint32_t y = 0; y < h; y++) {
      for (uint32_t x = 0; x < w; x++) {
        const size_t i = size_t(y) * w + x;
        // The second frame is every colour; the first is flat except at the
        // one pixel that will therefore be unchanged, and so maskable.
        const Rgba p = (all_colours || i == 256u) ? colour_at(i)
                                                  : Rgba{0, 0, 0, 255};
        uint8_t * px = base + y * stride + x * 4u;
        px[0] = p.r;
        px[1] = p.g;
        px[2] = p.b;
        px[3] = p.a;
      }
    }
    return r;
  };
  std::vector<GIMG_Raster *> frames{build(false), build(true)};
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);
  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 2u);
  // No entry to spare, so the frame cannot claim a transparent one.
  EXPECT_FALSE(blocks[1].transparent());
}

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

TEST(GifEncode, TheSaveOptionOverridesWhatTheDocumentDeclares) {
  // `gif_loop_count` is an override rather than the source of the count,
  // because its zero already means "repeat forever" and so has no spelling for
  // "not set".  Non-zero means the caller asked; zero means they did not, and
  // the document answers.  This is the first half - the document says 5 and
  // the caller says 7, and 7 wins.
  //
  // This test used to be called "a loop count survives a load and save when
  // the caller carries it", and carrying it was two lines of caller code
  // because the writer ignored the document entirely.  Those two lines are no
  // longer needed; the test below is what they were standing in for.
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
  options.gif_loop_count = 7;
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, &options, bytes,
                [](GIMG_Doc * doc) { gimg_doc_set_loop_count(doc, 5u); }),
      GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint32_t round_tripped = 0;
  EXPECT_EQ(gimg_doc_loop_count(again.doc(), &round_tripped), 1);
  EXPECT_EQ(round_tripped, 7u) << "the caller asked for 7 out loud";
}

TEST(GifEncode, ALoopCountSurvivesALoadAndSaveWithNoHelpFromTheCaller) {
  // The second half.  The writer used to ignore the document, so an animation
  // that asked to repeat five times came back asking to repeat for ever -
  // a round trip through this codec silently rewrote the instruction.
  // ImageMagick and Pillow both preserve it.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(4, 2, sixteen));
  frames.push_back(make_raster(4, 2, sixteen));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes,
                [](GIMG_Doc * doc) { gimg_doc_set_loop_count(doc, 5u); }),
      GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint32_t round_tripped = 0;
  ASSERT_EQ(gimg_doc_loop_count(again.doc(), &round_tripped), 1);
  EXPECT_EQ(round_tripped, 5u);
}

TEST(GifEncode, ADocumentWithNoLoopCountGetsNoNetscapeBlock) {
  // "Say nothing" is a different instruction from any count - browsers play
  // such a file once - and GIF, unlike APNG, can express it by leaving the
  // block out.  gimg_doc_clear_loop_count() has always documented exactly
  // this, and until now it was not true: every animation this writer produced
  // carried a count of zero, which says "repeat for ever".
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(4, 2, sixteen));
  frames.push_back(make_raster(4, 2, sixteen));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes, nullptr), GIMG_OK);

  const std::string all(bytes.begin(), bytes.end());
  EXPECT_EQ(all.find("NETSCAPE2.0"), std::string::npos)
      << "a count of zero is not the same as no count at all";

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint32_t round_tripped = 9;
  EXPECT_EQ(gimg_doc_loop_count(again.doc(), &round_tripped), 0);
}

TEST(GifEncode, ALoopCountOnAStillImageIsKeptRatherThanDropped) {
  // A count on a still image has nothing to repeat, but real files carry one -
  // gif_4x2_netscape_loop.gif is a single-frame GIF with the block - and this
  // codec's loader reports it.  Dropping on write what the accessor reports on
  // read is the silent loss the rest of this is about, so the frame count does
  // not gate the block.  Pillow keeps it; ImageMagick drops it and loses the
  // round trip.
  GIMG_Raster * raster = make_raster(4, 2, sixteen);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, nullptr, bytes,
                [](GIMG_Doc * doc) { gimg_doc_set_loop_count(doc, 5u); }),
      GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(again.doc()), 1u) << "still a still image";
  uint32_t round_tripped = 0;
  ASSERT_EQ(gimg_doc_loop_count(again.doc(), &round_tripped), 1);
  EXPECT_EQ(round_tripped, 5u);
}

TEST(GifEncode, ACountWiderThanTheFieldIsClampedRatherThanTruncated) {
  // 89a 26's count is two bytes and APNG's num_plays is four, so a document
  // that arrived from an APNG can carry more than GIF can say.  65536 written
  // into two bytes truncates to 0, which is the one value that means something
  // else entirely - "repeat for ever" instead of "repeat a great many times".
  // The largest count the field has is the nearest true thing.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(4, 2, sixteen));
  frames.push_back(make_raster(4, 2, sixteen));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes,
                [](GIMG_Doc * doc) { gimg_doc_set_loop_count(doc, 100000u); }),
      GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint32_t round_tripped = 0;
  ASSERT_EQ(gimg_doc_loop_count(again.doc(), &round_tripped), 1);
  EXPECT_EQ(round_tripped, 65535u) << "not 0, which would mean for ever";
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

TEST(GifEncode, TheBackgroundColourIsWrittenToTheScreenDescriptor) {
  // The writer used to put a zero in the Background Color Index always, which
  // made gimg_doc_set_background_color() a no-op that reported success: the
  // accessor gave the new colour and the file kept naming entry 0.  Only a
  // round trip showed it, which is why this asks the file and not the
  // accessor.
  //
  // Both ImageMagick and Pillow do this - `magick -background lime` repoints
  // the index at an entry it adds, and Pillow honours info["background"] - so
  // a GIF this library wrote was the odd one out.
  const Rgba want{0, 255, 0, 255}; // a green nothing in `sixteen` paints
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(8, 8, sixteen));
  frames.push_back(make_raster(8, 8, with_hole));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes,
                [&](GIMG_Doc * doc) {
                  const uint8_t rgba[4] = {want.r, want.g, want.b, want.a};
                  gimg_doc_set_background_color(doc, rgba);
                }),
      GIMG_OK);

  // The index must name something other than entry 0, which is the transparent
  // one the planner reserves and can never be a colour.
  ASSERT_GT(bytes.size(), 12u);
  EXPECT_NE(bytes[11], 0u) << "the Background Color Index of the new file";

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint8_t rgba[4] = {9, 9, 9, 9};
  ASSERT_EQ(gimg_doc_background_color(again.doc(), rgba), 1);
  EXPECT_EQ(Rgba({rgba[0], rgba[1], rgba[2], rgba[3]}), want);
}

TEST(GifEncode, ABackgroundColourNoPixelUsesIsAddedToTheTable) {
  // The colour a document declares need not be one the frames paint, and a
  // global table built only from the pixels will not have it.  Writing index 0
  // then would silently turn "the background is this colour" into "nothing is
  // behind this", which is a different statement and one the caller did not
  // make.  Adding an entry can take the table to the next power of two; that
  // is the price of saying something the file would otherwise not say.
  //
  // `sixteen` paints i*17, 255-i*9, i*5+3 for i of 0 to 15, so no pixel is
  // ever this.
  const Rgba want{1, 2, 3, 255};
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(8, 8, sixteen));
  frames.push_back(make_raster(8, 8, with_hole));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes,
                [&](GIMG_Doc * doc) {
                  const uint8_t rgba[4] = {want.r, want.g, want.b, want.a};
                  gimg_doc_set_background_color(doc, rgba);
                }),
      GIMG_OK);

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint8_t rgba[4] = {9, 9, 9, 9};
  ASSERT_EQ(gimg_doc_background_color(again.doc(), rgba), 1);
  EXPECT_EQ(Rgba({rgba[0], rgba[1], rgba[2], rgba[3]}), want)
      << "a colour no frame paints still has to reach the file";
}

TEST(GifEncode, ADocumentWithNoBackgroundNamesTheTransparentEntry) {
  // 89a 18 has no way to leave the field out, so "nothing is behind this" is
  // said by naming an entry that is marked transparent - entry 0, which is the
  // planner's mask index.  Reading the file back must give that statement
  // again and not a black background the document never declared.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(8, 8, sixteen));
  frames.push_back(make_raster(8, 8, with_hole));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes, nullptr), GIMG_OK);

  ASSERT_GT(bytes.size(), 12u);
  EXPECT_EQ(bytes[11], 0u) << "entry 0 is the transparent one";

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint8_t rgba[4] = {9, 9, 9, 9};
  ASSERT_EQ(gimg_doc_background_color(again.doc(), rgba), 1);
  EXPECT_EQ(rgba[3], 0u)
      << "an opaque colour here is a background nobody declared";
}

TEST(GifEncode, ABackgroundAtAlphaZeroIsTheSameStatementAsNone) {
  // gimg_doc_background_color() reports a transparent entry as the colour at
  // alpha 0, so a document that came from such a file carries one.  Writing it
  // as an opaque entry would turn "nothing is behind this" into a colour on a
  // round trip through this codec, which is the bug in the other direction
  // from the one above.
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(8, 8, sixteen));
  frames.push_back(make_raster(8, 8, with_hole));
  ASSERT_NE(frames[0], nullptr);
  ASSERT_NE(frames[1], nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes,
                [](GIMG_Doc * doc) {
                  const uint8_t rgba[4] = {200u, 100u, 50u, 0u};
                  gimg_doc_set_background_color(doc, rgba);
                }),
      GIMG_OK);

  ASSERT_GT(bytes.size(), 12u);
  EXPECT_EQ(bytes[11], 0u) << "alpha 0 names the transparent entry";
}

TEST(GifEncode, APixelAspectRatioSurvivesALoadAndSave) {
  // The writer used to put a zero in the Pixel Aspect Ratio byte always, so a
  // file that declared a non-square pixel came back declaring nothing - the
  // loader read it, the accessor reported it, and saving threw it away.
  //
  // It needs no save option, which is what separates it from the loop count.
  // `gif_loop_count`'s zero already means "forever", so it had no spelling for
  // "unset" and could not read the document without changing what an existing
  // caller's zero meant.  Zero here means "no information given" in the format
  // and "declares nothing" in the document, which is the same statement twice.
  Loaded source;
  ASSERT_EQ(source.load("gif_8x8_pixel_aspect.gif"), GIMG_OK);
  uint32_t num = 0, den = 0;
  ASSERT_EQ(gimg_doc_pixel_aspect_ratio(source.doc(), &num, &den), 1);
  ASSERT_EQ(num, 128u);
  ASSERT_EQ(den, 64u);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  ASSERT_EQ(gimg_doc_save(source.doc(), out, "gif", nullptr, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),
      static_cast<const uint8_t *>(data) + size);
  gimg_stream_destroy(out);

  // The byte itself, so a failure says which half broke.
  ASSERT_GT(bytes.size(), 12u);
  EXPECT_EQ(bytes[12], 113u) << "the Pixel Aspect Ratio byte of the new file";

  Loaded again;
  ASSERT_EQ(again.load_bytes(bytes), GIMG_OK);
  uint32_t rn = 0, rd = 0;
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(again.doc(), &rn, &rd), 1);
  EXPECT_EQ(rn, 128u);
  EXPECT_EQ(rd, 64u);
}

TEST(GifEncode, ARatioTheFormatCannotHoldIsWrittenAsNoneRatherThanTheNearest) {
  // 89a 18 can only express (N + 15) / 64 for N of 1 to 255, which is 16/64 to
  // 270/64.  A document declaring something outside that gets a zero byte:
  // saying nothing is true, and rounding to the nearest expressible ratio
  // would put a number in the file that the caller never asked for and could
  // not tell apart from one they did.
  for (const auto & pair : std::vector<std::pair<uint32_t, uint32_t>>{
           {100u, 1u}, {1u, 100u}, {5u, 1u}, {1u, 5u}}) {
    GIMG_Raster * raster = make_raster(8, 8, sixteen);
    ASSERT_NE(raster, nullptr);
    std::vector<uint8_t> bytes;
    ASSERT_EQ(save_raster(raster, nullptr, bytes,
                  [&](GIMG_Doc * doc) {
                    gimg_doc_set_pixel_aspect_ratio(doc, pair.first,
                        pair.second);
                  }),
        GIMG_OK);
    ASSERT_GT(bytes.size(), 12u);
    EXPECT_EQ(bytes[12], 0u)
        << pair.first << ":" << pair.second << " is not expressible";
  }
  // The edges of the range are expressible, and are written.
  for (const auto & pair : std::vector<std::pair<uint32_t, uint32_t>>{
           {16u, 64u}, {270u, 64u}, {1u, 4u}, {2u, 1u}}) {
    GIMG_Raster * raster = make_raster(8, 8, sixteen);
    ASSERT_NE(raster, nullptr);
    std::vector<uint8_t> bytes;
    ASSERT_EQ(save_raster(raster, nullptr, bytes,
                  [&](GIMG_Doc * doc) {
                    gimg_doc_set_pixel_aspect_ratio(doc, pair.first,
                        pair.second);
                  }),
        GIMG_OK);
    ASSERT_GT(bytes.size(), 12u);
    EXPECT_NE(bytes[12], 0u)
        << pair.first << ":" << pair.second << " is expressible";
  }
}

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

TEST(GifComments, KeepRawOnlyWritesNoDescription) {
  // The description is the document's, not the file's, so the policy that
  // keeps only what the file arrived with must not write it - the same rule
  // the PNG and BMP writers follow.  That arm had never run: every comment
  // test here used the default policy or DROP_ALL.
  //
  // The control is the same document under PRESERVE_ALL, which must carry it.
  // Without that, a policy that dropped everything for some other reason
  // would look identical.
  const char * kText = "kept by the document, not by the file";
  auto with_description = [kText](GIMG_Doc * doc) {
    GIMG_Meta_Common * common = nullptr;
    ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &common), GIMG_OK);
    ASSERT_EQ(gimg_meta_common_set_description(common, kText), GIMG_OK);
  };

  GIMG_Save_Options preserve;
  memset(&preserve, 0, sizeof(preserve));
  preserve.metadata_policy = GIMG_META_PRESERVE_ALL;
  std::vector<uint8_t> kept;
  ASSERT_EQ(save_with(&preserve, kept, with_description), GIMG_OK);
  const std::vector<std::string> in_kept = comments_in(kept);
  ASSERT_EQ(in_kept.size(), 1u) << "control: PRESERVE_ALL writes it";
  EXPECT_EQ(in_kept[0], kText);

  GIMG_Save_Options raw_only;
  memset(&raw_only, 0, sizeof(raw_only));
  raw_only.metadata_policy = GIMG_META_KEEP_RAW_ONLY;
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_with(&raw_only, bytes, with_description), GIMG_OK);
  EXPECT_TRUE(comments_in(bytes).empty())
      << "the description came from the document; KEEP_RAW_ONLY writes only "
         "what the file arrived with";
}

// A changed rectangle with unchanged pixels inside it.
//
// The writer crops each frame to the rectangle that changed, so in the usual
// case every pixel it writes is a pixel that moved.  Two changes at opposite
// corners leave a rectangle that spans almost the whole canvas with almost
// nothing in it changed, and those pixels have to be written as the
// transparent index - masked, so the frame below shows through - rather than
// repainted with the colour they already have.
//
// That masking had never run.  Every animation in the suite changes one
// contiguous block or nothing at all, and both of those crop to a rectangle
// with no unchanged pixel in it.
TEST(GifEncode, UnchangedPixelsInsideAChangedRectangleAreMasked) {
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(24, 16, patch_base));
  frames.push_back(make_raster(24, 16, patch_two_corners));
  for (GIMG_Raster * f : frames) { ASSERT_NE(f, nullptr); }
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);

  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_EQ(blocks[1].x, 1u);
  EXPECT_EQ(blocks[1].y, 1u);
  EXPECT_EQ(blocks[1].w, 22u) << "the rectangle spans both corners";
  EXPECT_EQ(blocks[1].h, 14u);
  EXPECT_TRUE(blocks[1].transparent())
      << "the pixels inside it that did not change have to be masked, which "
         "takes a transparent index";

  // What it draws is the test.  Masking that painted the wrong pixels, or
  // that masked one that had changed, gives a different second frame.
  publish("masked_interior_24x16.gif", bytes,
      {expectation(24, 16, patch_base), expectation(24, 16, patch_two_corners)});
}

// The same masking, on the path that builds a palette per frame.
//
// The writer has two frame planners: one for when a single global table holds
// every colour in the animation, and one for when it does not and each frame
// carries its own.  They do the same masking, written twice, and only the
// global one had ever run it - every multi-frame fixture here fits a global
// table.
//
// Two frames needing 302 colours between them do not, which sends both down
// the local-palette planner; one pixel in five changes, so four in five are
// unchanged and inside the rectangle that has to be written.
TEST(GifEncode, MaskingAlsoHappensWhenEachFrameCarriesItsOwnPalette) {
  std::vector<GIMG_Raster *> frames;
  frames.push_back(make_raster(32, 16, many_colours_first));
  frames.push_back(make_raster(32, 16, many_colours_second));
  for (GIMG_Raster * f : frames) { ASSERT_NE(f, nullptr); }
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_frames(frames, nullptr, bytes), GIMG_OK);

  EXPECT_EQ(global_table_entries(bytes), 0u)
      << "302 colours do not fit one table, which is what puts these frames "
         "on the per-frame path this test is for";
  const std::vector<Block> blocks = image_blocks(bytes);
  ASSERT_EQ(blocks.size(), 2u);
  EXPECT_TRUE(blocks[1].local_table) << "so each frame carries its own";
  EXPECT_TRUE(blocks[1].transparent())
      << "the unchanged pixels inside the rectangle are masked";

  publish("masked_local_palette_32x16.gif", bytes,
      {expectation(32, 16, many_colours_first),
          expectation(32, 16, many_colours_second)});
}

namespace {

/** Every pixel transparent. */
Rgba all_transparent(uint32_t x, uint32_t y) {
  (void)x;
  (void)y;
  return Rgba{0, 0, 0, 0};
}

} // namespace

// A frame with no colours in it at all.
//
// GIF 89a 18: a colour table holds at least two entries, and its size is
// written as a power of two, so a frame whose every pixel is transparent
// still needs a table - there is an index to point the Graphic Control
// Extension's transparent colour at.  The writer counts the colours it saw,
// which is none, and then makes the count one.  That had never run: nothing
// in the suite saved a frame with nothing opaque in it.
TEST(GifEncode, AFrameWithNothingOpaqueStillGetsAColourTable) {
  GIMG_Raster * raster = make_raster(8, 4, all_transparent);
  ASSERT_NE(raster, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save_raster(raster, nullptr, bytes), GIMG_OK)
      << "a frame of nothing is a frame, not a refusal";

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t y = 0; y < 4u; y++) {
    for (uint32_t x = 0; x < 8u; x++) {
      EXPECT_EQ(img.at(x, y).a, 0u) << "at (" << x << "," << y << ")";
    }
  }
  publish("all_transparent_8x4.gif", bytes,
      {expectation(8, 4, all_transparent)});
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
