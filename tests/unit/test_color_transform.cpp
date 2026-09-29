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

TEST(ColorTransform, NonEightBitRefused) {
  GIMG_Raster * src = nullptr;
  ASSERT_EQ(gimg_raster_create(1, 1, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED,
                nullptr, 0, &src),
      GIMG_OK);
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ASSERT_TRUE(gcol_color_info_set_gamut(&ci, GCOL_PRIMARIES_SRGB));
  ci.transfer = GCOL_TRANSFER_SRGB;
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Color_Transform_Options opts;
  gimg_color_transform_options_srgb(&opts);
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_ops_transform_color(src, &opts, &out), GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(src);
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
