/**
 * @file
 *
 * Unit tests for the gamut table: identification, lookup, and the invariant
 * the table's design rests on.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cmath>
#include <cstring>
#include <ghoti.io/image/color.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

/** Every named space, taken from the enum so a new one is swept by default. */
std::vector<GIMG_Primaries> every_named() {
  std::vector<GIMG_Primaries> out;
  for (int i = GIMG_PRIMARIES_UNKNOWN + 1; i < GIMG_PRIMARIES_COUNT; i++) {
    out.push_back((GIMG_Primaries)i);
  }
  return out;
}

bool close(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

} // namespace

// The invariant gimg_gamut_identify() depends on, asserted rather than
// assumed: a gamut carrying no white point - which is every BMP V4 header,
// since the format has nowhere to put one - is matched on its three primaries
// alone, and that is only unambiguous while no two named spaces share them.
//
// Theatrical DCI-P3 is the space that would break it: its primaries are
// Display P3's and its white point is not D65.  Adding it must fail here
// rather than silently make a white-point-less P3 file resolve to whichever
// of the two the loop reaches first.
TEST(ColorGamut, NoTwoNamedGamutsShareTheirPrimaries) {
  const auto named = every_named();
  ASSERT_GE(named.size(), 2u) << "a one-entry table asserts nothing";
  for (size_t i = 0; i < named.size(); i++) {
    GIMG_Gamut a;
    ASSERT_TRUE(gimg_gamut_named(named[i], &a)) << "named " << named[i];
    for (size_t j = i + 1; j < named.size(); j++) {
      GIMG_Gamut b;
      ASSERT_TRUE(gimg_gamut_named(named[j], &b)) << "named " << named[j];
      const bool same = close(a.red.x, b.red.x, GIMG_GAMUT_TOLERANCE_DEFAULT) &&
          close(a.red.y, b.red.y, GIMG_GAMUT_TOLERANCE_DEFAULT) &&
          close(a.green.x, b.green.x, GIMG_GAMUT_TOLERANCE_DEFAULT) &&
          close(a.green.y, b.green.y, GIMG_GAMUT_TOLERANCE_DEFAULT) &&
          close(a.blue.x, b.blue.x, GIMG_GAMUT_TOLERANCE_DEFAULT) &&
          close(a.blue.y, b.blue.y, GIMG_GAMUT_TOLERANCE_DEFAULT);
      EXPECT_FALSE(same)
          << "spaces " << named[i] << " and " << named[j]
          << " share their primaries, so a gamut with no white point cannot "
             "be named from them; gimg_gamut_identify() must be taught to "
             "refuse rather than guess";
    }
  }
}

TEST(ColorGamut, EveryNamedSpaceRoundTrips) {
  for (GIMG_Primaries p : every_named()) {
    GIMG_Gamut g;
    ASSERT_TRUE(gimg_gamut_named(p, &g)) << "named " << p;
    EXPECT_EQ(gimg_gamut_identify(&g, GIMG_GAMUT_TOLERANCE_DEFAULT), p);
  }
}

TEST(ColorGamut, UnknownAndTheSentinelHaveNoCoordinates) {
  GIMG_Gamut g;
  EXPECT_FALSE(gimg_gamut_named(GIMG_PRIMARIES_UNKNOWN, &g));
  EXPECT_FALSE(gimg_gamut_named(GIMG_PRIMARIES_COUNT, &g));
  EXPECT_FALSE(gimg_gamut_named((GIMG_Primaries)999, &g));
  EXPECT_FALSE(gimg_gamut_named(GIMG_PRIMARIES_SRGB, nullptr));
}

TEST(ColorGamut, NullGamutIsUnknown) {
  EXPECT_EQ(gimg_gamut_identify(nullptr, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_UNKNOWN);
}

// A BMP V4 header states three endpoints and no white point at all, so this
// is the shape the BMP reader hands over.
TEST(ColorGamut, AGamutWithNoWhitePointIsNamedFromItsPrimaries) {
  GIMG_Gamut g;
  ASSERT_TRUE(gimg_gamut_named(GIMG_PRIMARIES_ADOBE_RGB, &g));
  g.white.x = 0.0;
  g.white.y = 0.0;
  EXPECT_EQ(gimg_gamut_identify(&g, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_ADOBE_RGB);
}

// But a white point that *is* stated has to agree: sRGB's primaries under a
// D50 white are not sRGB, and naming them so would be the rounding this model
// exists to avoid.
TEST(ColorGamut, AWhitePointThatDisagreesRefusesTheMatch) {
  GIMG_Gamut g;
  ASSERT_TRUE(gimg_gamut_named(GIMG_PRIMARIES_SRGB, &g));
  g.white.x = 0.3457; // D50.
  g.white.y = 0.3585;
  EXPECT_EQ(gimg_gamut_identify(&g, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_UNKNOWN);
}

TEST(ColorGamut, ToleranceBoundsTheMatch) {
  GIMG_Gamut g;
  ASSERT_TRUE(gimg_gamut_named(GIMG_PRIMARIES_SRGB, &g));
  g.green.x += 0.0009;
  EXPECT_EQ(gimg_gamut_identify(&g, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_SRGB)
      << "inside the default tolerance";
  g.green.x += 0.0004;
  EXPECT_EQ(gimg_gamut_identify(&g, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_UNKNOWN)
      << "outside it";
  EXPECT_EQ(gimg_gamut_identify(&g, 0.01), GIMG_PRIMARIES_SRGB)
      << "a caller may widen it";
}

// Zero would otherwise mean "match only exactly", which is a bound no file
// written through a decimal encoding could meet.
TEST(ColorGamut, ANonPositiveToleranceMeansTheDefault) {
  GIMG_Gamut g;
  ASSERT_TRUE(gimg_gamut_named(GIMG_PRIMARIES_DISPLAY_P3, &g));
  g.red.x += 0.0005;
  for (double t : {0.0, -1.0, std::nan("")}) {
    EXPECT_EQ(gimg_gamut_identify(&g, t), GIMG_PRIMARIES_DISPLAY_P3)
        << "tolerance " << t;
  }
}

// GIMG_Color_Info carries doubles that nothing polices, so a NaN can arrive
// here from a malformed file; it must fail every comparison rather than pass
// them all.
TEST(ColorGamut, NaNCoordinatesMatchNothing) {
  GIMG_Gamut g;
  ASSERT_TRUE(gimg_gamut_named(GIMG_PRIMARIES_SRGB, &g));
  g.blue.y = std::nan("");
  EXPECT_EQ(gimg_gamut_identify(&g, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_UNKNOWN);
}

TEST(ColorGamut, SetGamutStoresCoordinates) {
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ASSERT_FALSE(ci.gamut_stated);

  ASSERT_TRUE(gimg_color_info_set_gamut(&ci, GIMG_PRIMARIES_BT2020));
  EXPECT_TRUE(ci.gamut_stated);
  EXPECT_EQ(gimg_gamut_identify(&ci.gamut, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_BT2020);

  // Setting an unnameable space clears rather than leaving the previous one
  // standing, which would be the worst of both.
  EXPECT_FALSE(gimg_color_info_set_gamut(&ci, GIMG_PRIMARIES_UNKNOWN));
  EXPECT_FALSE(ci.gamut_stated);
  EXPECT_EQ(gimg_gamut_identify(&ci.gamut, GIMG_GAMUT_TOLERANCE_DEFAULT),
      GIMG_PRIMARIES_UNKNOWN);

  EXPECT_FALSE(gimg_color_info_set_gamut(nullptr, GIMG_PRIMARIES_SRGB));
}

// The default must zero every field, including ones added after it was
// written: a designated initializer naming each field silently leaves a new
// one uninitialized, which is why this is a memset and why this test reads
// the fields the redesign added.
TEST(ColorGamut, DefaultStatesNothing) {
  GIMG_Color_Info ci;
  std::memset(&ci, 0xA5, sizeof(ci));
  gimg_color_info_default(&ci);
  EXPECT_FALSE(ci.gamut_stated);
  EXPECT_EQ(ci.transfer, GIMG_TRANSFER_UNKNOWN);
  EXPECT_EQ(ci.reference, GIMG_REFERENCE_UNKNOWN);
  EXPECT_EQ(ci.white_luminance, 0.0);
  EXPECT_EQ(ci.gamma_value, 0.0);
  EXPECT_EQ(ci.icc_bytes, nullptr);
  EXPECT_EQ(ci.icc_size, 0u);
  EXPECT_EQ(ci.icc_linked_path, nullptr);
  EXPECT_EQ(ci.cmyk_polarity, GIMG_CMYK_POLARITY_UNKNOWN);
  for (int i = 0; i < GIMG_TRANSFER_PARAM_COUNT; i++) {
    EXPECT_EQ(ci.transfer_params[i], 0.0) << "term " << i;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
