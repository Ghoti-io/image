/**
 * @file
 *
 * Float rasters, against every public operation and every encoder.
 *
 * GIMG_PIXEL_RGBAF32 and GIMG_PIXEL_GRAYF32 arrived after every operation in
 * this library was written for integers, and each of those decides what it
 * can read by bits_per_channel.  A float raster is safe only if each one
 * either moves its bytes without reading them or refuses it - an operation
 * that took 32 bits for a UINT32, or a sixteen-bit float for a UNORM16, would
 * produce plausible numbers and no error.  So this file states the whole
 * surface: the byte movers keep every bit (checked with values no integer
 * path would preserve: negatives, values past 1, a NaN, -0.0), and every
 * operation that interprets a sample, and every codec that writes, says
 * GIMG_ERR_UNSUPPORTED.  A new operation or codec lands in one list or the
 * other, or the sweep over the registry fails.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

#include "../registry_sweep.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

const GIMG_Pixel_Format * const kFormats[] = {
    &GIMG_PIXEL_RGBAF32, &GIMG_PIXEL_GRAYF32};

uint8_t channels(const GIMG_Pixel_Format * f) {
  return f->channel_count;
}

// A value per (x, y, c) that no integer conversion would carry through:
// negatives, values above 1, tiny and huge magnitudes, and in one sample
// each a NaN with a payload and a negative zero.
float sample(uint32_t x, uint32_t y, uint8_t c) {
  if (x == 1u && y == 0u && c == 0u) {
    uint32_t bits = 0x7fc01234u;
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
  }
  if (x == 2u && y == 1u && c == 0u) {
    return -0.0f;
  }
  const float base = (float)(x * 7u + y * 13u + c * 3u + 1u);
  return ((x + c) % 2u ? -1.0f : 1.0f) * std::ldexp(base, (int)(y * 9u) - 20);
}

GIMG_Raster * make(const GIMG_Pixel_Format * f, uint32_t w, uint32_t h) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, f, GIMG_RASTER_OWNED, nullptr, 0, &r) !=
      GIMG_OK) {
    return nullptr;
  }
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < h; y++) {
    float * row = (float *)(void *)(p + (size_t)y * stride);
    for (uint32_t x = 0; x < w; x++) {
      for (uint8_t c = 0; c < channels(f); c++) {
        row[(size_t)x * channels(f) + c] = sample(x, y, c);
      }
    }
  }
  return r;
}

uint32_t bits_at(const GIMG_Raster * r, uint32_t x, uint32_t y, uint8_t c) {
  const unsigned char * p =
      (const unsigned char *)gimg_raster_pixels_const(r) +
      (size_t)y * gimg_raster_stride_bytes(r);
  uint32_t out;
  std::memcpy(&out,
      p + ((size_t)x * channels(gimg_raster_format(r)) + c) * sizeof(float),
      sizeof out);
  return out;
}

uint32_t bits_of(float f) {
  uint32_t out;
  std::memcpy(&out, &f, sizeof out);
  return out;
}

// That @p r holds the source sample (sx(x,y), sy(x,y)) at every (x, y).
template <typename SX, typename SY>
void expect_moved(const GIMG_Raster * r, SX sx, SY sy, const char * what) {
  const GIMG_Pixel_Format * f = gimg_raster_format(r);
  ASSERT_EQ(f->channel_type, GIMG_CHANNEL_FLOAT) << what;
  size_t bad = 0;
  for (uint32_t y = 0; y < gimg_raster_height(r); y++) {
    for (uint32_t x = 0; x < gimg_raster_width(r); x++) {
      for (uint8_t c = 0; c < channels(f); c++) {
        if (bits_at(r, x, y, c) != bits_of(sample(sx(x, y), sy(x, y), c))) {
          bad++;
        }
      }
    }
  }
  EXPECT_EQ(bad, 0u) << what;
}

} // namespace

TEST(FloatRaster, FormatsAreFourBytesPerChannel) {
  EXPECT_EQ(gimg_raster_bytes_per_pixel(&GIMG_PIXEL_RGBAF32), 16u);
  EXPECT_EQ(gimg_raster_bytes_per_pixel(&GIMG_PIXEL_GRAYF32), 4u);
  EXPECT_EQ(GIMG_PIXEL_RGBAF32.alpha_form, GIMG_ALPHA_STRAIGHT);
  EXPECT_EQ(GIMG_PIXEL_GRAYF32.alpha_form, GIMG_ALPHA_NONE);
  for (const GIMG_Pixel_Format * f : kFormats) {
    GIMG_Raster * r = make(f, 5, 3);
    ASSERT_NE(r, nullptr);
    expect_moved(r, [](uint32_t x, uint32_t) { return x; },
        [](uint32_t, uint32_t y) { return y; }, "create");
    gimg_raster_destroy(r);
  }
}

// Every width an integer path would misread is refused at the door.
TEST(FloatRaster, OnlyThirtyTwoBitFloatIsAccepted) {
  for (uint8_t bits : {8, 12, 16, 24, 64}) {
    GIMG_Pixel_Format f = GIMG_PIXEL_RGBAF32;
    for (int i = 0; i < 4; i++) {
      f.bits_per_channel[i] = bits;
    }
    GIMG_Raster * r = nullptr;
    EXPECT_EQ(gimg_raster_create(4, 4, &f, GIMG_RASTER_OWNED, nullptr, 0, &r),
        GIMG_ERR_UNSUPPORTED)
        << (int)bits;
    EXPECT_EQ(r, nullptr);
  }
  GIMG_Pixel_Format mixed = GIMG_PIXEL_RGBAF32;
  mixed.bits_per_channel[3] = 16;
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(4, 4, &mixed, GIMG_RASTER_OWNED, nullptr, 0,
                &r),
      GIMG_ERR_UNSUPPORTED);
  GIMG_Pixel_Format gray16 = GIMG_PIXEL_GRAYF32;
  gray16.bits_per_channel[0] = 16;
  EXPECT_EQ(gimg_raster_create(4, 4, &gray16, GIMG_RASTER_OWNED, nullptr, 0,
                &r),
      GIMG_ERR_UNSUPPORTED);
}

TEST(FloatRaster, ByteMoversKeepEveryBit) {
  const uint32_t W = 5, H = 3;
  for (const GIMG_Pixel_Format * f : kFormats) {
    auto fresh = [&]() { return make(f, W, H); };
    GIMG_Raster * r;

    r = fresh();
    ASSERT_EQ(gimg_ops_flip_horizontal(r), GIMG_OK);
    expect_moved(r, [&](uint32_t x, uint32_t) { return W - 1 - x; },
        [](uint32_t, uint32_t y) { return y; }, "flip_horizontal");
    gimg_raster_destroy(r);

    r = fresh();
    ASSERT_EQ(gimg_ops_flip_vertical(r), GIMG_OK);
    expect_moved(r, [](uint32_t x, uint32_t) { return x; },
        [&](uint32_t, uint32_t y) { return H - 1 - y; }, "flip_vertical");
    gimg_raster_destroy(r);

    r = fresh();
    ASSERT_EQ(gimg_ops_rotate_180(r), GIMG_OK);
    expect_moved(r, [&](uint32_t x, uint32_t) { return W - 1 - x; },
        [&](uint32_t, uint32_t y) { return H - 1 - y; }, "rotate_180");
    gimg_raster_destroy(r);

    r = fresh();
    ASSERT_EQ(gimg_ops_rotate_90_cw(r), GIMG_OK);
    ASSERT_EQ(gimg_raster_width(r), H);
    expect_moved(r, [](uint32_t, uint32_t y) { return y; },
        [&](uint32_t x, uint32_t) { return H - 1 - x; }, "rotate_90_cw");
    gimg_raster_destroy(r);

    r = fresh();
    ASSERT_EQ(gimg_ops_rotate_90_ccw(r), GIMG_OK);
    expect_moved(r, [&](uint32_t, uint32_t y) { return W - 1 - y; },
        [](uint32_t x, uint32_t) { return x; }, "rotate_90_ccw");
    gimg_raster_destroy(r);

    r = fresh();
    ASSERT_EQ(gimg_ops_apply_orientation(r, GIMG_ORIENTATION_TRANSPOSE),
        GIMG_OK);
    expect_moved(r, [](uint32_t, uint32_t y) { return y; },
        [](uint32_t x, uint32_t) { return x; }, "transpose");
    gimg_raster_destroy(r);

    GIMG_Raster * src = fresh();
    GIMG_Raster * out = nullptr;
    ASSERT_EQ(gimg_ops_crop(src, 1, 1, 3, 2, &out), GIMG_OK);
    expect_moved(out, [](uint32_t x, uint32_t) { return x + 1; },
        [](uint32_t, uint32_t y) { return y + 1; }, "crop");
    gimg_raster_destroy(out);

    out = nullptr;
    ASSERT_EQ(gimg_raster_copy(src, &out), GIMG_OK);
    expect_moved(out, [](uint32_t x, uint32_t) { return x; },
        [](uint32_t, uint32_t y) { return y; }, "copy");
    EXPECT_TRUE(gimg_ops_raster_equal(src, out));
    // One flipped bit is a difference, NaN payload included.
    unsigned char * p = (unsigned char *)gimg_raster_pixels(out);
    p[sizeof(float) * channels(f)] ^= 1u;
    EXPECT_FALSE(gimg_ops_raster_equal(src, out));
    gimg_raster_destroy(out);

    out = nullptr;
    ASSERT_EQ(gimg_ops_convert_pixel_format(src, f, &out), GIMG_OK);
    expect_moved(out, [](uint32_t x, uint32_t) { return x; },
        [](uint32_t, uint32_t y) { return y; }, "convert to itself");
    gimg_raster_destroy(out);

    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(gimg_raster_create(W, H, f, GIMG_RASTER_OWNED, nullptr, 0, &dst),
        GIMG_OK);
    ASSERT_EQ(gimg_ops_composite(dst, src, 0, 0, GIMG_COMPOSITE_SOURCE),
        GIMG_OK);
    expect_moved(dst, [](uint32_t x, uint32_t) { return x; },
        [](uint32_t, uint32_t y) { return y; }, "composite SOURCE");
    gimg_raster_destroy(dst);
    gimg_raster_destroy(src);
  }
}

TEST(FloatRaster, EveryInterpretingOperationRefuses) {
  for (const GIMG_Pixel_Format * f : kFormats) {
    const bool rgba = f->channel_model == GIMG_CHANNEL_RGBA;
    GIMG_Raster * src = make(f, 4, 4);
    ASSERT_NE(src, nullptr);
    GIMG_Raster * out = nullptr;

    GIMG_Resize_Options ro;
    gimg_resize_options_default(&ro);
    EXPECT_EQ(gimg_ops_resize(src, 2, 2, &ro, &out), GIMG_ERR_UNSUPPORTED);
    EXPECT_EQ(out, nullptr);

    const GIMG_Pixel_Format * narrow =
        rgba ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_GRAY8;
    EXPECT_EQ(gimg_ops_convert_pixel_format(src, narrow, &out),
        GIMG_ERR_UNSUPPORTED);
    EXPECT_EQ(out, nullptr);
    for (uint8_t bits : {8, 12, 16}) {
      EXPECT_EQ(gimg_ops_convert_bit_depth(src, bits, &out),
          GIMG_ERR_UNSUPPORTED)
          << (int)bits;
      EXPECT_EQ(out, nullptr);
    }

    GIMG_Color_Transform_Options co;
    gimg_color_transform_options_srgb(&co);
    EXPECT_EQ(gimg_ops_transform_color(src, &co, &out), GIMG_ERR_UNSUPPORTED);
    EXPECT_EQ(out, nullptr);

    EXPECT_EQ(gimg_alpha_premultiply(src), GIMG_ERR_UNSUPPORTED);
    EXPECT_EQ(gimg_alpha_unpremultiply(src), GIMG_ERR_UNSUPPORTED);

    size_t count = 0;
    bool exact = false;
    EXPECT_EQ(gimg_ops_count_colors(src, 16, &count, &exact),
        GIMG_ERR_UNSUPPORTED);
    GIMG_Palette pal;
    std::memset(&pal, 0, sizeof pal);
    EXPECT_EQ(gimg_ops_palette_from_raster(src, 256, &pal),
        GIMG_ERR_UNSUPPORTED);
    GIMG_Quantize_Options qo;
    std::memset(&qo, 0, sizeof qo);
    const GIMG_Raster * const list[] = {src};
    EXPECT_EQ(gimg_ops_palette_build(list, 1, &qo, &pal),
        GIMG_ERR_UNSUPPORTED);
    pal.count = 2;
    EXPECT_EQ(gimg_ops_palette_apply(src, &pal, GIMG_DITHER_NONE, &out),
        GIMG_ERR_UNSUPPORTED);
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(gimg_ops_quantize(src, &qo, &out, &pal), GIMG_ERR_UNSUPPORTED);
    EXPECT_EQ(out, nullptr);

    GIMG_Raster * dst = make(f, 4, 4);
    EXPECT_EQ(gimg_ops_composite(dst, src, 0, 0, GIMG_COMPOSITE_OVER),
        GIMG_ERR_UNSUPPORTED);
    // Refused means untouched.
    expect_moved(dst, [](uint32_t x, uint32_t) { return x; },
        [](uint32_t, uint32_t y) { return y; }, "refused composite OVER");
    gimg_raster_destroy(dst);
    expect_moved(src, [](uint32_t x, uint32_t) { return x; },
        [](uint32_t, uint32_t y) { return y; }, "after every refusal");
    gimg_raster_destroy(src);
  }
}

// No encoder in the registry can write a float sample, and each must say so
// rather than narrow it or write the bytes as integers.
TEST(FloatRaster, EveryEncoderRefuses) {
  size_t writers = 0;
  for (const gimg_test::SweptCodec & c : gimg_test::swept_codecs()) {
    if (!c.writes()) {
      continue;
    }
    writers++;
    for (const GIMG_Pixel_Format * f : kFormats) {
      GIMG_Raster * r = make(f, 4, 4);
      ASSERT_NE(r, nullptr);
      GIMG_Doc * doc = nullptr;
      ASSERT_EQ(gimg_doc_from_raster(r, &doc), GIMG_OK);
      gimg_raster_destroy(r);
      GIMG_Stream * out = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
      GIMG_Save_Report report;
      std::memset(&report, 0, sizeof report);
      EXPECT_EQ(gimg_doc_save(doc, out, c.name.c_str(), nullptr, &report),
          GIMG_ERR_UNSUPPORTED)
          << c.name << (f == &GIMG_PIXEL_RGBAF32 ? " RGBAF32" : " GRAYF32");
      gimg_stream_destroy(out);
      gimg_doc_destroy(doc);
    }
  }
  // Seven codecs write today; fewer means the sweep stopped seeing some.
  EXPECT_GE(writers, 7u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
