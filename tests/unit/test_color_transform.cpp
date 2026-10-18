/**
 * @file
 *
 * Explicit colour transforms via libs/color.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/color/color.h>
#include <ghoti.io/color/icc.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "../oracle_gate.h"

namespace {

std::vector<uint8_t> slurp(const std::string & path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string icc_path(const std::string & name) {
#ifdef GIMG_TEST_DATA_ROOT
  return std::string(GIMG_TEST_DATA_ROOT) + "/icc/" + name;
#else
  return oracle_gate::repo_root() + "/tests/data/icc/" + name;
#endif
}

std::vector<uint8_t> load_icc(const std::string & name) {
  return slurp(icc_path(name));
}

GIMG_Raster * make_rgba8(uint32_t w, uint32_t h) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &r) != GIMG_OK) {
    return nullptr;
  }
  return r;
}

GIMG_Raster * make_cmyk8(uint32_t w, uint32_t h) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, nullptr, 0,
          &r) != GIMG_OK) {
    return nullptr;
  }
  return r;
}

} // namespace

TEST(ColorTransform, OptionsDefaultZerosReserved) {
  GIMG_Color_Transform_Options o;
  memset(&o, 0xA5, sizeof(o));
  gimg_color_transform_options_default(&o);
  EXPECT_EQ(o.intent, GCOL_INTENT_RELATIVE_COLORIMETRIC);
  EXPECT_EQ(o.flags, 0u);
  EXPECT_EQ(o.dest.transfer, GCOL_TRANSFER_UNKNOWN);
  for (size_t i = 0; i < sizeof(o._reserved); i++) {
    EXPECT_EQ(o._reserved[i], 0u);
  }
}

TEST(ColorTransform, OptionsSrgbIsMatrixReady) {
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_srgb(&o);
  EXPECT_EQ(o.dest.transfer, GCOL_TRANSFER_SRGB);
  EXPECT_TRUE(o.dest.primaries_stated);
  EXPECT_TRUE(o.dest.white_stated);
  EXPECT_EQ(gcol_gamut_identify(&o.dest.gamut, GCOL_GAMUT_TOLERANCE_DEFAULT),
      GCOL_PRIMARIES_SRGB);
}

/**
 * swap_rg.icc names sRGB's green as its red. Transforming a red pixel toward
 * sRGB must land in the green channel — the same gate color's unit tests use.
 */
TEST(ColorTransform, SwapRgTurnsRedIntoGreen) {
  auto bytes = load_icc("swap_rg.icc");
  ASSERT_FALSE(bytes.empty()) << "run tests/data/icc/generate.py";

  GIMG_Raster * src = make_rgba8(1, 1);
  ASSERT_NE(src, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  p[0] = 255;
  p[1] = 0;
  p[2] = 0;
  p[3] = 255;

  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = bytes.data();
  ci.icc_size = bytes.size();
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_OK);
  ASSERT_NE(out, nullptr);

  const unsigned char * q =
      (const unsigned char *)gimg_raster_pixels_const(out);
  EXPECT_GT(q[1], 200) << "green";
  EXPECT_LT(q[0], 80) << "red";
  EXPECT_LT(q[2], 80) << "blue";
  EXPECT_EQ(q[3], 255);

  const GCOL_Color_Info * got = gimg_raster_color_info_const(out);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->transfer, GCOL_TRANSFER_SRGB);

  gimg_raster_destroy(out);
  gimg_raster_destroy(src);
}

/**
 * Carriage: without transform, swap_rg bytes do not move the samples.
 */
TEST(ColorTransform, WithoutTransformPixelsStayPut) {
  auto bytes = load_icc("swap_rg.icc");
  ASSERT_FALSE(bytes.empty());

  GIMG_Raster * src = make_rgba8(1, 1);
  ASSERT_NE(src, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  p[0] = 255;
  p[1] = 0;
  p[2] = 0;
  p[3] = 255;

  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = bytes.data();
  ci.icc_size = bytes.size();
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  EXPECT_EQ(p[0], 255);
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);

  GIMG_Raster * copy = nullptr;
  ASSERT_EQ(gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, &copy),
      GIMG_OK);
  const unsigned char * q =
      (const unsigned char *)gimg_raster_pixels_const(copy);
  EXPECT_EQ(q[0], 255);
  EXPECT_EQ(q[1], 0);
  EXPECT_EQ(q[2], 0);

  gimg_raster_destroy(copy);
  gimg_raster_destroy(src);
}

/**
 * Adobe RGB → sRGB must match littleCMS (Pillow ImageCms) on the same probes
 * verify_icc.py uses. Off-by-one is allowed: ImageCms and our matrix path
 * round independently when encoding 8-bit.
 *
 * Measured ImageCms 2.16 / Pillow 11.1.0:
 *   (160, 96, 64) → (180, 96, 60)
 *   (64, 160, 96) → (0, 161, 92)
 */
TEST(ColorTransform, MatrixAdobeRgbToSrgbMatchesImageCms) {
  auto bytes = load_icc("adobergb_g22.icc");
  ASSERT_FALSE(bytes.empty());

  struct Probe {
    unsigned char in[3];
    unsigned char want[3];
  };
  const Probe probes[] = {
      {{160, 96, 64}, {180, 96, 60}},
      {{64, 160, 96}, {0, 161, 92}},
  };

  for (const Probe & probe : probes) {
    GIMG_Raster * src = make_rgba8(1, 1);
    ASSERT_NE(src, nullptr);
    unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
    p[0] = probe.in[0];
    p[1] = probe.in[1];
    p[2] = probe.in[2];
    p[3] = 255;

    GCOL_Color_Info ci;
    gcol_color_info_default(&ci);
    ci.icc_bytes = bytes.data();
    ci.icc_size = bytes.size();
    ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

    GIMG_Color_Transform_Options opts;
    gimg_color_transform_options_srgb(&opts);
    GIMG_Raster * out = nullptr;
    ASSERT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_OK)
        << "in (" << (int)probe.in[0] << "," << (int)probe.in[1] << ","
        << (int)probe.in[2] << ")";
    ASSERT_NE(out, nullptr);
    const unsigned char * q =
        (const unsigned char *)gimg_raster_pixels_const(out);
    for (int c = 0; c < 3; c++) {
      EXPECT_NEAR(q[c], probe.want[c], 2)
          << "channel " << c << " for in (" << (int)probe.in[0] << ","
          << (int)probe.in[1] << "," << (int)probe.in[2] << ")";
    }
    EXPECT_EQ(q[3], 255);

    gimg_raster_destroy(out);
    gimg_raster_destroy(src);
  }
}

TEST(ColorTransform, StatedGamutWithoutIccWorks) {
  GIMG_Raster * src = make_rgba8(1, 1);
  ASSERT_NE(src, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  p[0] = 128;
  p[1] = 64;
  p[2] = 32;
  p[3] = 200;

  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ASSERT_TRUE(gcol_color_info_set_gamut(&ci, GCOL_PRIMARIES_DISPLAY_P3));
  ci.transfer = GCOL_TRANSFER_SRGB;
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_OK);
  ASSERT_NE(out, nullptr);
  const unsigned char * q =
      (const unsigned char *)gimg_raster_pixels_const(out);
  EXPECT_EQ(q[3], 200) << "alpha passes through";

  gimg_raster_destroy(out);
  gimg_raster_destroy(src);
}

TEST(ColorTransform, UntaggedRgbIsRefused) {
  GIMG_Raster * src = make_rgba8(1, 1);
  ASSERT_NE(src, nullptr);
  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = (GIMG_Raster *)0x1;
  EXPECT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(src);
}

TEST(ColorTransform, NullOptionsRefused) {
  GIMG_Raster * src = make_rgba8(1, 1);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * out = (GIMG_Raster *)0x1;
  EXPECT_EQ(gimg_ops_transform_color(src, nullptr, &out), GIMG_ERR_INTERNAL);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(src);
}

namespace {

GIMG_Raster * make(const GIMG_Pixel_Format * f, uint32_t w, uint32_t h) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, f, GIMG_RASTER_OWNED, nullptr, 0, &r) !=
      GIMG_OK) {
    return nullptr;
  }
  return r;
}

void tag_icc(GIMG_Raster * r, const std::vector<uint8_t> & icc,
    GCOL_CMYK_Polarity polarity = GCOL_CMYK_POLARITY_UNKNOWN) {
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = icc.data();
  ci.icc_size = icc.size();
  ci.cmyk_polarity = polarity;
  ASSERT_EQ(gimg_raster_set_color_info(r, &ci), GIMG_OK);
}

void tag_srgb(GIMG_Raster * r, GCOL_Transfer transfer = GCOL_TRANSFER_SRGB) {
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ASSERT_TRUE(gcol_color_info_set_gamut(&ci, GCOL_PRIMARIES_SRGB));
  ci.transfer = transfer;
  ASSERT_EQ(gimg_raster_set_color_info(r, &ci), GIMG_OK);
}

uint16_t u16_at(const GIMG_Raster * r, size_t i) {
  uint16_t v;
  std::memcpy(&v, (const uint8_t *)gimg_raster_pixels_const(r) + i * 2u, 2);
  return v;
}

float f32_at(const GIMG_Raster * r, size_t i) {
  float v;
  std::memcpy(&v, (const uint8_t *)gimg_raster_pixels_const(r) + i * 4u, 4);
  return v;
}

const uint8_t kRamp[8][4] = {{255, 0, 0, 255}, {0, 255, 0, 255},
    {0, 0, 255, 128}, {200, 100, 50, 255}, {12, 240, 99, 0},
    {128, 128, 128, 255}, {1, 2, 3, 255}, {254, 253, 252, 7}};

/** The same eight pixels through the 8-bit path, as the reference. */
std::vector<uint8_t> via_eight_bits(const std::vector<uint8_t> & icc) {
  GIMG_Raster * src = make(&GIMG_PIXEL_RGBA8, 8, 1);
  std::memcpy(gimg_raster_pixels(src), kRamp, sizeof kRamp);
  tag_icc(src, icc);
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_srgb(&o);
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_ops_transform_color(src, &o, &out), GIMG_OK);
  std::vector<uint8_t> v((const uint8_t *)gimg_raster_pixels_const(out),
      (const uint8_t *)gimg_raster_pixels_const(out) + 32);
  gimg_raster_destroy(out);
  gimg_raster_destroy(src);
  return v;
}

} // namespace

// Sixteen and twelve bits go through the same transform as eight, keep
// their depth, and agree with the 8-bit result to within its rounding.  The
// 16-bit result is not an 8-bit one widened: its low byte varies.
TEST(ColorTransform, DeepRgbKeepsItsDepthAndAgreesWithEightBits) {
  auto icc = load_icc("swap_rg.icc");
  ASSERT_FALSE(icc.empty());
  const std::vector<uint8_t> ref = via_eight_bits(icc);
  struct Depth {
    const GIMG_Pixel_Format * f;
    double full;
  } depths[] = {{&GIMG_PIXEL_RGBA16, 65535.0}, {&GIMG_PIXEL_RGBA12, 4095.0}};
  for (const Depth & d : depths) {
    GIMG_Raster * src = make(d.f, 8, 1);
    uint16_t * p = (uint16_t *)gimg_raster_pixels(src);
    for (int i = 0; i < 32; i++) {
      p[i] = (uint16_t)(kRamp[i / 4][i % 4] / 255.0 * d.full + 0.5);
    }
    tag_icc(src, icc);
    GIMG_Color_Transform_Options o;
    gimg_color_transform_options_srgb(&o);
    GIMG_Raster * out = nullptr;
    ASSERT_EQ(gimg_ops_transform_color(src, &o, &out), GIMG_OK);
    ASSERT_EQ(gimg_raster_format(out)->bits_per_channel[0],
        d.f->bits_per_channel[0]);
    bool low_bits = false;
    for (int i = 0; i < 32; i++) {
      const uint16_t v = u16_at(out, (size_t)i);
      EXPECT_LE(v, (uint16_t)d.full);
      EXPECT_NEAR(v / d.full * 255.0, ref[(size_t)i], 1.0) << i;
      if (d.full > 4095.0 && v % 257u != 0u) {
        low_bits = true;
      }
    }
    // Alpha is copied, exactly.
    for (int i = 3; i < 32; i += 4) {
      EXPECT_EQ(u16_at(out, (size_t)i), p[i]);
    }
    if (d.full > 4095.0) {
      EXPECT_TRUE(low_bits) << "16-bit output looks like widened 8-bit";
    }
    gimg_raster_destroy(out);
    gimg_raster_destroy(src);
  }
}

TEST(ColorTransform, Cmyk16AgreesWithCmyk8) {
  auto icc = load_icc("cmyk_mft2.icc");
  ASSERT_FALSE(icc.empty());
  const uint8_t ink[4][4] = {
      {255, 255, 255, 255}, {0, 255, 255, 255}, {128, 64, 32, 200},
      {10, 200, 90, 0}};
  GIMG_Raster * a = make(&GIMG_PIXEL_CMYK8, 4, 1);
  GIMG_Raster * b = make(&GIMG_PIXEL_CMYK16, 4, 1);
  std::memcpy(gimg_raster_pixels(a), ink, sizeof ink);
  uint16_t * w = (uint16_t *)gimg_raster_pixels(b);
  for (int i = 0; i < 16; i++) {
    w[i] = (uint16_t)(ink[i / 4][i % 4] * 257u);
  }
  tag_icc(a, icc, GCOL_CMYK_POLARITY_INK);
  tag_icc(b, icc, GCOL_CMYK_POLARITY_INK);
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_srgb(&o);
  GIMG_Raster *oa = nullptr, *ob = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(a, &o, &oa), GIMG_OK);
  ASSERT_EQ(gimg_ops_transform_color(b, &o, &ob), GIMG_OK);
  ASSERT_EQ(gimg_raster_format(ob)->channel_model, GIMG_CHANNEL_RGBA);
  ASSERT_EQ(gimg_raster_format(ob)->bits_per_channel[0], 16u);
  const uint8_t * q = (const uint8_t *)gimg_raster_pixels_const(oa);
  for (int i = 0; i < 16; i++) {
    EXPECT_NEAR(u16_at(ob, (size_t)i) / 257.0, q[i], 1.0) << i;
  }
  for (int i = 3; i < 16; i += 4) {
    EXPECT_EQ(u16_at(ob, (size_t)i), 65535u); // Opaque, as CMYK8 gives 255.
  }
  gimg_raster_destroy(oa);
  gimg_raster_destroy(ob);
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

// A gray raster under a gray profile becomes neutral sRGB.  littleCMS 2.16
// answers 64, 128 and 200 of gray_g22.icc as 62, 129 and 201
// (libs/color tests/data/transform/lcms.txt, measured 2026-10-17).
TEST(ColorTransform, GrayProfileToSrgbIsNeutralAsLittleCmsHasIt) {
  auto icc = load_icc("gray_g22.icc");
  ASSERT_FALSE(icc.empty());
  const uint8_t in[5] = {0, 64, 128, 200, 255};
  const uint8_t lcms[5] = {0, 62, 129, 201, 255};
  GIMG_Raster * src = make(&GIMG_PIXEL_GRAY8, 5, 1);
  std::memcpy(gimg_raster_pixels(src), in, 5);
  tag_icc(src, icc);
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_srgb(&o);
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(src, &o, &out), GIMG_OK);
  ASSERT_EQ(gimg_raster_format(out)->channel_model, GIMG_CHANNEL_RGBA);
  const uint8_t * q = (const uint8_t *)gimg_raster_pixels_const(out);
  for (int i = 0; i < 5; i++) {
    for (int c = 0; c < 3; c++) {
      EXPECT_NEAR(q[i * 4 + c], lcms[i], 1) << i << " " << c;
    }
    EXPECT_EQ(q[i * 4 + 3], 255);
  }
  gimg_raster_destroy(out);

  // Sixteen bits too, into RGBA16.
  GIMG_Raster * deep = make(&GIMG_PIXEL_GRAY16, 5, 1);
  uint16_t * w = (uint16_t *)gimg_raster_pixels(deep);
  for (int i = 0; i < 5; i++) {
    w[i] = (uint16_t)(in[i] * 257u);
  }
  tag_icc(deep, icc);
  ASSERT_EQ(gimg_ops_transform_color(deep, &o, &out), GIMG_OK);
  ASSERT_EQ(gimg_raster_format(out)->bits_per_channel[0], 16u);
  for (int i = 0; i < 5; i++) {
    EXPECT_NEAR(u16_at(out, (size_t)i * 4u) / 257.0, lcms[i], 1.0) << i;
  }
  gimg_raster_destroy(out);
  gimg_raster_destroy(deep);
  gimg_raster_destroy(src);
}

// A gray raster under an RGB description is that space's neutrals: it
// transforms exactly as the RGBA8 raster with R = G = B does.
TEST(ColorTransform, GrayUnderAnRgbDescriptionIsItsNeutrals) {
  auto icc = load_icc("adobergb_g22.icc");
  ASSERT_FALSE(icc.empty());
  GIMG_Raster * g = make(&GIMG_PIXEL_GRAY8, 256, 1);
  GIMG_Raster * c = make(&GIMG_PIXEL_RGBA8, 256, 1);
  uint8_t * gp = (uint8_t *)gimg_raster_pixels(g);
  uint8_t * cp = (uint8_t *)gimg_raster_pixels(c);
  for (int i = 0; i < 256; i++) {
    gp[i] = (uint8_t)i;
    cp[i * 4] = cp[i * 4 + 1] = cp[i * 4 + 2] = (uint8_t)i;
    cp[i * 4 + 3] = 255;
  }
  tag_icc(g, icc);
  tag_icc(c, icc);
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_srgb(&o);
  GIMG_Raster *og = nullptr, *oc = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(g, &o, &og), GIMG_OK);
  ASSERT_EQ(gimg_ops_transform_color(c, &o, &oc), GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(og, oc));
  gimg_raster_destroy(og);
  gimg_raster_destroy(oc);
  gimg_raster_destroy(g);
  gimg_raster_destroy(c);
}

// RGB into a gray destination is luminance - littleCMS 2.16 has pure sRGB
// red, green and blue as 129, 219 and 71 of gray_g22.icc - and only an
// opaque picture can go there, because gray has nowhere to put alpha.
TEST(ColorTransform, RgbToGrayIsLuminanceAndRefusesTranslucency) {
  auto icc = load_icc("gray_g22.icc");
  ASSERT_FALSE(icc.empty());
  GIMG_Raster * src = make(&GIMG_PIXEL_RGBA8, 5, 1);
  const uint8_t px[5][4] = {{255, 0, 0, 255}, {0, 255, 0, 255},
      {0, 0, 255, 255}, {255, 255, 255, 255}, {0, 0, 0, 255}};
  const uint8_t lcms[5] = {129, 219, 71, 255, 0};
  std::memcpy(gimg_raster_pixels(src), px, sizeof px);
  tag_srgb(src);
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_default(&o);
  o.dest.icc_bytes = icc.data();
  o.dest.icc_size = icc.size();
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(src, &o, &out), GIMG_OK);
  ASSERT_EQ(gimg_raster_format(out)->channel_model, GIMG_CHANNEL_GRAY);
  const uint8_t * q = (const uint8_t *)gimg_raster_pixels_const(out);
  for (int i = 0; i < 5; i++) {
    EXPECT_NEAR(q[i], lcms[i], 1) << i;
  }
  gimg_raster_destroy(out);

  ((uint8_t *)gimg_raster_pixels(src))[4 * 2 + 3] = 254;
  out = nullptr;
  EXPECT_EQ(gimg_ops_transform_color(src, &o, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(src);
}

// A float raster stays float and unbounded: linear 2.0 and 179 into sRGB are
// 1.353 and 9.106, and -0.1 is -1.292, which is littleCMS 2.16's answer for
// the same transform.  This is what a LogLuv decode is transformed with.
TEST(ColorTransform, FloatStaysFloatAndUnbounded) {
  GIMG_Raster * src = make(&GIMG_PIXEL_RGBAF32, 2, 1);
  const float in[8] = {2.0f, 179.0f, -0.1f, 0.25f, 0.5f, 0.25f, 0.125f, 1.0f};
  std::memcpy(gimg_raster_pixels(src), in, sizeof in);
  tag_srgb(src, GCOL_TRANSFER_LINEAR);
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_srgb(&o);
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(src, &o, &out), GIMG_OK);
  ASSERT_EQ(gimg_raster_format(out)->channel_type, GIMG_CHANNEL_FLOAT);
  const double want[8] = {1.35325599, 9.10596466, -1.29200006, 0.25,
      0.735356987, 0.537098706, 0.388572872, 1.0};
  for (int i = 0; i < 8; i++) {
    EXPECT_NEAR(f32_at(out, (size_t)i), want[i],
        2e-4 * std::fmax(1.0, std::fabs(want[i])))
        << i;
  }
  gimg_raster_destroy(out);

  GIMG_Raster * gray = make(&GIMG_PIXEL_GRAYF32, 1, 1);
  const float g = 3.0f;
  std::memcpy(gimg_raster_pixels(gray), &g, sizeof g);
  auto icc = load_icc("gray_g22.icc");
  tag_icc(gray, icc);
  ASSERT_EQ(gimg_ops_transform_color(gray, &o, &out), GIMG_OK);
  // littleCMS: gray_g22 3.0 is sRGB 2.832.
  EXPECT_NEAR(f32_at(out, 0), 2.83207655, 2e-4 * 2.84);
  EXPECT_EQ(f32_at(out, 3), 1.0f);
  gimg_raster_destroy(out);
  gimg_raster_destroy(gray);
  gimg_raster_destroy(src);
}

TEST(ColorTransform, WhatHasNoAnswerIsStillRefused) {
  GIMG_Color_Transform_Options o;
  gimg_color_transform_options_srgb(&o);
  GIMG_Raster * out = nullptr;

  // Float into CMYK: there is no float CMYK raster to hold it.
  auto cmyk = load_icc("cmyk_mft2.icc");
  GIMG_Raster * f = make(&GIMG_PIXEL_RGBAF32, 1, 1);
  tag_srgb(f);
  GIMG_Color_Transform_Options to_cmyk;
  gimg_color_transform_options_default(&to_cmyk);
  to_cmyk.dest.icc_bytes = cmyk.data();
  to_cmyk.dest.icc_size = cmyk.size();
  to_cmyk.dest.cmyk_polarity = GCOL_CMYK_POLARITY_INK;
  EXPECT_EQ(gimg_ops_transform_color(f, &to_cmyk, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(f);

  // Colour samples under a gray profile say nothing about the colour.
  auto gray = load_icc("gray_g22.icc");
  GIMG_Raster * c = make(&GIMG_PIXEL_RGBA8, 1, 1);
  tag_icc(c, gray);
  EXPECT_EQ(gimg_ops_transform_color(c, &o, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(c);

  // Untagged gray, like untagged RGB, has no space to start from.
  GIMG_Raster * g = make(&GIMG_PIXEL_GRAY8, 1, 1);
  EXPECT_EQ(gimg_ops_transform_color(g, &o, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(g);
}

TEST(ColorTransform, CmykWithoutPolarityRefused) {
  auto bytes = load_icc("cmyk_mft2.icc");
  ASSERT_FALSE(bytes.empty()) << "cmyk_mft2.icc missing from tests/data/icc/";

  GIMG_Raster * src = make_cmyk8(1, 1);
  ASSERT_NE(src, nullptr);
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = bytes.data();
  ci.icc_size = bytes.size();
  ci.cmyk_polarity = GCOL_CMYK_POLARITY_UNKNOWN;
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(src);
}

TEST(ColorTransform, CmykIccToSrgb) {
  auto bytes = load_icc("cmyk_mft2.icc");
  ASSERT_FALSE(bytes.empty());

  GIMG_Raster * src = make_cmyk8(1, 1);
  ASSERT_NE(src, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  /* INK polarity (Adobe/JPEG): 255 = no ink. Full white in, expect bright out. */
  p[0] = 255;
  p[1] = 255;
  p[2] = 255;
  p[3] = 255;

  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = bytes.data();
  ci.icc_size = bytes.size();
  ci.cmyk_polarity = GCOL_CMYK_POLARITY_INK;
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(gimg_raster_format(out)->channel_model, GIMG_CHANNEL_RGBA);
  const unsigned char * q =
      (const unsigned char *)gimg_raster_pixels_const(out);
  EXPECT_EQ(q[3], 255);
  /* Mid-grey CMYK LUT fixtures are coarse; require the result be bright. */
  EXPECT_GT(q[0] + q[1] + q[2], 400);

  gimg_raster_destroy(out);
  gimg_raster_destroy(src);
}

TEST(ColorTransform, UntaggedCmykRefused) {
  GIMG_Raster * src = make_cmyk8(1, 1);
  ASSERT_NE(src, nullptr);
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.cmyk_polarity = GCOL_CMYK_POLARITY_INK;
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(src);
}

/**
 * Resolver-supplied bytes are ordinary icc_bytes on the raster. Transforming
 * them is how PROFILE_LINKED becomes useful after icc_resolver returns.
 */
TEST(ColorTransform, ResolverSuppliedBytesBehaveLikeEmbedded) {
  auto bytes = load_icc("swap_rg.icc");
  ASSERT_FALSE(bytes.empty());

  GIMG_Raster * src = make_rgba8(1, 1);
  ASSERT_NE(src, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  p[0] = 255;
  p[1] = 0;
  p[2] = 0;
  p[3] = 255;

  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  /* Same field the BMP loader fills after a successful icc_resolver. */
  ci.icc_bytes = bytes.data();
  ci.icc_size = bytes.size();
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_OK);
  const unsigned char * q =
      (const unsigned char *)gimg_raster_pixels_const(out);
  EXPECT_GT(q[1], 200);

  gimg_raster_destroy(out);
  gimg_raster_destroy(src);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
