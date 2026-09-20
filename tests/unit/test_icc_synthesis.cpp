/**
 * @file
 *
 * Can a JPEG carry a color model that arrived without a profile?
 *
 * Every other place this library states a color space, it is repeating one:
 * a profile read from one file written verbatim into the next, a gamut named
 * by a BMP's endpoints coming back out as a PNG's cHRM. A JPEG has neither
 * gAMA nor cHRM nor cICP; APP2 is the only place it can name a color space
 * at all. So a raster that knew its primaries and its gamma but carried no
 * profile had both silently dropped on the way in, and that was the one gap
 * left in the conversion matrix.
 *
 * The profile written for such a raster is manufactured rather than
 * repeated, which makes two things worth asserting that are not worth
 * asserting anywhere else: that it says what the raster said, and that it is
 * never written over a profile the source actually carried.
 *
 * These read the profile out of the saved file rather than calling the
 * builder, because the bytes in the file are the thing that has to be right.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

constexpr double kS15Fixed16 = 65536.0;
/** D50, the PCS illuminant, as ICC.1:2001-04 section 6.1.6 encodes it. */
constexpr double kD50X = 0x0000F6D6 / kS15Fixed16;
constexpr double kD50Y = 0x00010000 / kS15Fixed16;
constexpr double kD50Z = 0x0000D32D / kS15Fixed16;

uint32_t be32(const uint8_t * p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
      ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** A raster whose color model is stated but whose profile is absent. */
GIMG_Raster * stating(GIMG_Primaries primaries, GIMG_Transfer transfer,
    double gamma, const GIMG_Pixel_Format * format = &GIMG_PIXEL_RGBA8,
    GIMG_Rendering_Intent intent = GIMG_INTENT_PERCEPTUAL) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(
          16, 16, format, GIMG_RASTER_OWNED, nullptr, 0, &raster) != GIMG_OK ||
      !raster) {
    return nullptr;
  }
  std::memset(gimg_raster_pixels(raster), 0x60,
      gimg_raster_stride_bytes(raster) * 16u);
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.primaries = primaries;
  ci.transfer = transfer;
  ci.gamma_value = gamma;
  ci.intent = intent;
  if (gimg_raster_set_color_info(raster, &ci) != GIMG_OK) {
    gimg_raster_destroy(raster);
    return nullptr;
  }
  return raster;
}

/** Save a document (taking ownership of @p raster) as @p format. */
::testing::AssertionResult save_as(GIMG_Raster * raster, const char * format,
    std::vector<uint8_t> & out,
    GIMG_Meta_Policy policy = GIMG_META_PRESERVE_ALL) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK || !doc) {
    gimg_raster_destroy(raster);
    return ::testing::AssertionFailure() << "gimg_doc_create";
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * stream = nullptr;
  if (gimg_stream_create_memory_output(&stream) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return ::testing::AssertionFailure() << "gimg_stream_create_memory_output";
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, stream, format, &opts, &report);
  if (r == GIMG_OK) {
    const void * bytes = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &bytes, &size);
    out.assign(static_cast<const uint8_t *>(bytes),
        static_cast<const uint8_t *>(bytes) + size);
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r == GIMG_OK
      ? ::testing::AssertionSuccess()
      : ::testing::AssertionFailure() << "save as " << format << ": " << r;
}

/** The ICC profile a saved JPEG's APP2 segments carry, reassembled. */
std::vector<uint8_t> profile_in(const std::vector<uint8_t> & jpeg) {
  std::vector<uint8_t> profile;
  size_t i = 2; // Past SOI.
  while (i + 4 <= jpeg.size() && jpeg[i] == 0xFF) {
    uint8_t marker = jpeg[i + 1];
    if (marker == 0xDA || marker == 0xD9) {
      break;
    }
    size_t len = ((size_t)jpeg[i + 2] << 8) | (size_t)jpeg[i + 3];
    if (len < 2 || i + 2 + len > jpeg.size()) {
      break;
    }
    const uint8_t * payload = jpeg.data() + i + 4;
    size_t payload_len = len - 2;
    if (marker == 0xE2 && payload_len > 14 &&
        std::memcmp(payload, "ICC_PROFILE\0", 12) == 0) {
      profile.insert(profile.end(), payload + 14, payload + payload_len);
    }
    i += 2 + len;
  }
  return profile;
}

/** Offset and size of @p sig in @p profile's tag table, or {0, 0}. */
std::pair<size_t, size_t> tag_in(
    const std::vector<uint8_t> & profile, const char * sig) {
  if (profile.size() < 132) {
    return {0, 0};
  }
  uint32_t count = be32(profile.data() + 128);
  if (count > 64 || 132u + (count * 12u) > profile.size()) {
    return {0, 0};
  }
  for (uint32_t t = 0; t < count; t++) {
    const uint8_t * e = profile.data() + 132 + (t * 12u);
    if (std::memcmp(e, sig, 4) == 0) {
      size_t off = be32(e + 4);
      size_t size = be32(e + 8);
      if (off + size <= profile.size()) {
        return {off, size};
      }
    }
  }
  return {0, 0};
}

/** The three components of an XYZType tag, or all zero when it is absent. */
struct Xyz {
  double x = 0, y = 0, z = 0;
};

Xyz xyz_tag(const std::vector<uint8_t> & profile, const char * sig) {
  auto [off, size] = tag_in(profile, sig);
  if (size < 20 || std::memcmp(profile.data() + off, "XYZ ", 4) != 0) {
    return {};
  }
  return {(double)(int32_t)be32(profile.data() + off + 8) / kS15Fixed16,
      (double)(int32_t)be32(profile.data() + off + 12) / kS15Fixed16,
      (double)(int32_t)be32(profile.data() + off + 16) / kS15Fixed16};
}

} // namespace

/**
 * A raster that states a gamut and a curve must reach a JPEG saying so.
 *
 * This is the gap: before, a save wrote no APP2 at all for such a raster and
 * the statement was lost with nothing to mark its passing.
 */
TEST(JpegSynthesizedIcc, AStatedColorModelBecomesAProfile) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(save_as(
      stating(GIMG_PRIMARIES_ADOBE_RGB, GIMG_TRANSFER_GAMMA, 2.2), "jpeg",
      jpeg));

  std::vector<uint8_t> profile = profile_in(jpeg);
  ASSERT_GE(profile.size(), 128u)
      << "a JPEG saved from a raster that names its color space must carry a "
         "profile: APP2 is the only place the format can say it";
  EXPECT_EQ(std::memcmp(profile.data() + 36, "acsp", 4), 0)
      << "every ICC profile carries 'acsp' at byte 36";
  EXPECT_EQ(be32(profile.data()), (uint32_t)profile.size())
      << "the profile's declared size must match what the segments carried";
  EXPECT_EQ(std::memcmp(profile.data() + 16, "RGB ", 4), 0);
  EXPECT_EQ(std::memcmp(profile.data() + 20, "XYZ ", 4), 0);
}

/**
 * The profile must state the gamut the raster named, not some other one.
 *
 * Adobe RGB differs from sRGB only in its green primary, so a writer that
 * ignored the raster and emitted sRGB would still look plausible; the green
 * colorant is what tells the two apart.
 */
TEST(JpegSynthesizedIcc, TheProfileNamesTheGamutTheRasterNamed) {
  std::vector<uint8_t> adobe_jpeg, srgb_jpeg;
  ASSERT_TRUE(save_as(
      stating(GIMG_PRIMARIES_ADOBE_RGB, GIMG_TRANSFER_GAMMA, 2.2), "jpeg",
      adobe_jpeg));
  ASSERT_TRUE(save_as(
      stating(GIMG_PRIMARIES_SRGB, GIMG_TRANSFER_SRGB, 0.0), "jpeg",
      srgb_jpeg));

  Xyz adobe_green = xyz_tag(profile_in(adobe_jpeg), "gXYZ");
  Xyz srgb_green = xyz_tag(profile_in(srgb_jpeg), "gXYZ");
  ASSERT_GT(adobe_green.y, 0.0);
  ASSERT_GT(srgb_green.y, 0.0);
  EXPECT_NEAR(adobe_green.y, 0.62567, 0.0001)
      << "Adobe RGB's green luminance, ICC's D50-adapted value";
  EXPECT_NEAR(srgb_green.y, 0.71687, 0.0001) << "sRGB's green luminance";
  EXPECT_GT(srgb_green.y - adobe_green.y, 0.05)
      << "the two gamuts must not come out as the same profile";
}

/**
 * The colorants must sum to the media white point.
 *
 * This is the property that makes a set of matrix colorants well formed: the
 * matrix has to map device white to the profile's white. Asserting it here
 * checks the tabulated values mean what they claim, which comparing them to a
 * copy of themselves would not.
 */
TEST(JpegSynthesizedIcc, ColorantsSumToTheMediaWhitePoint) {
  for (auto primaries : {GIMG_PRIMARIES_SRGB, GIMG_PRIMARIES_ADOBE_RGB}) {
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(
        save_as(stating(primaries, GIMG_TRANSFER_GAMMA, 2.2), "jpeg", jpeg));
    std::vector<uint8_t> profile = profile_in(jpeg);

    Xyz white = xyz_tag(profile, "wtpt");
    EXPECT_NEAR(white.x, kD50X, 1e-6) << "the PCS illuminant is D50";
    EXPECT_NEAR(white.y, kD50Y, 1e-6);
    EXPECT_NEAR(white.z, kD50Z, 1e-6);

    Xyz r = xyz_tag(profile, "rXYZ");
    Xyz g = xyz_tag(profile, "gXYZ");
    Xyz b = xyz_tag(profile, "bXYZ");
    EXPECT_NEAR(r.x + g.x + b.x, kD50X, 0.0002) << "primaries " << primaries;
    EXPECT_NEAR(r.y + g.y + b.y, kD50Y, 0.0002) << "primaries " << primaries;
    EXPECT_NEAR(r.z + g.z + b.z, kD50Z, 0.0002) << "primaries " << primaries;
  }
}

/**
 * A stated gamma must reach the profile as that gamma.
 *
 * A curveType of one sample point holds a u8Fixed8Number exponent, which is
 * the whole of what GIMG_TRANSFER_GAMMA says.
 */
TEST(JpegSynthesizedIcc, AStatedGammaReachesTheToneCurve) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(save_as(
      stating(GIMG_PRIMARIES_SRGB, GIMG_TRANSFER_GAMMA, 1.8), "jpeg", jpeg));
  std::vector<uint8_t> profile = profile_in(jpeg);

  auto [off, size] = tag_in(profile, "rTRC");
  ASSERT_GE(size, 14u);
  EXPECT_EQ(std::memcmp(profile.data() + off, "curv", 4), 0);
  ASSERT_EQ(be32(profile.data() + off + 8), 1u)
      << "a single gamma value is one sample point";
  unsigned raw = ((unsigned)profile.data()[off + 12] << 8) |
      (unsigned)profile.data()[off + 13];
  EXPECT_NEAR((double)raw / 256.0, 1.8, 0.005);
}

/**
 * All three tone curves must describe the same curve.
 *
 * They share one block of tag data, which the spec allows and which keeps a
 * tabulated sRGB curve from being stored three times. Sharing is an easy
 * thing to get wrong in a way no single-channel check would notice.
 */
TEST(JpegSynthesizedIcc, EveryChannelGetsTheSameToneCurve) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(save_as(
      stating(GIMG_PRIMARIES_SRGB, GIMG_TRANSFER_SRGB, 0.0), "jpeg", jpeg));
  std::vector<uint8_t> profile = profile_in(jpeg);

  auto r = tag_in(profile, "rTRC");
  auto g = tag_in(profile, "gTRC");
  auto b = tag_in(profile, "bTRC");
  ASSERT_GT(r.second, 0u);
  EXPECT_EQ(r, g);
  EXPECT_EQ(r, b);
  EXPECT_EQ(std::memcmp(profile.data() + r.first, "curv", 4), 0);
  EXPECT_GT(be32(profile.data() + r.first + 8), 1u)
      << "the sRGB curve is piecewise and has to be tabulated";
}

/**
 * A profile the source carried must be written, not one built over the top.
 *
 * The library repeats color statements wherever it can; synthesis is only for
 * where there is nothing to repeat. A raster carrying both a profile and a
 * stated model must come out carrying its own profile.
 */
TEST(JpegSynthesizedIcc, ACarriedProfileIsNotReplaced) {
  std::vector<uint8_t> carried(600, 0);
  carried[0] = 0;
  carried[1] = 0;
  carried[2] = 2;
  carried[3] = 88; // Declared size 600.
  std::memcpy(carried.data() + 36, "acsp", 4);
  for (size_t i = 40; i < carried.size(); i++) {
    carried[i] = (uint8_t)((i * 31u) & 0xFFu);
  }

  GIMG_Raster * raster =
      stating(GIMG_PRIMARIES_ADOBE_RGB, GIMG_TRANSFER_GAMMA, 2.2);
  ASSERT_NE(raster, nullptr);
  GIMG_Color_Info ci = *gimg_raster_color_info_const(raster);
  ci.icc_bytes = carried.data();
  ci.icc_size = carried.size();
  ASSERT_EQ(gimg_raster_set_color_info(raster, &ci), GIMG_OK);

  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(save_as(raster, "jpeg", jpeg));
  EXPECT_EQ(profile_in(jpeg), carried)
      << "the profile the raster carried must survive byte for byte, rather "
         "than being replaced by one built from the same raster's model";
}

/**
 * Half a color model must be written as nothing at all.
 *
 * Primaries without a transfer function, or the reverse, cannot become a
 * matrix/TRC profile without inventing the missing half. Inventing it would
 * put a statement in the file that nobody made.
 */
TEST(JpegSynthesizedIcc, HalfAColorModelSaysNothing) {
  struct {
    const char * what;
    GIMG_Primaries primaries;
    GIMG_Transfer transfer;
  } const cases[] = {
      {"a gamut with no curve", GIMG_PRIMARIES_ADOBE_RGB,
          GIMG_TRANSFER_UNKNOWN},
      {"a curve with no gamut", GIMG_PRIMARIES_UNKNOWN, GIMG_TRANSFER_SRGB},
      {"neither", GIMG_PRIMARIES_UNKNOWN, GIMG_TRANSFER_UNKNOWN},
  };
  for (const auto & c : cases) {
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(save_as(stating(c.primaries, c.transfer, 2.2), "jpeg", jpeg))
        << c.what;
    EXPECT_TRUE(profile_in(jpeg).empty()) << c.what;
  }
}

/**
 * A gray JPEG must not be given an RGB profile.
 *
 * What is synthesized is a three-colorant matrix profile, which describes an
 * RGB frame and nothing else. A gray frame would need a profile of a
 * different kind, and a wrong profile is worse than none.
 */
TEST(JpegSynthesizedIcc, AGrayFrameGetsNoRgbProfile) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(save_as(stating(GIMG_PRIMARIES_SRGB, GIMG_TRANSFER_SRGB, 0.0,
                          &GIMG_PIXEL_GRAY8),
      "jpeg", jpeg));
  EXPECT_TRUE(profile_in(jpeg).empty())
      << "an RGB matrix profile does not describe a one-component frame";
}

/**
 * The rendering intent the raster states must reach the profile header.
 *
 * It is the one field of the color model that is copied straight through
 * rather than turned into a tag, which makes it the easiest to leave at its
 * default without anyone noticing.
 */
TEST(JpegSynthesizedIcc, TheStatedRenderingIntentReachesTheHeader) {
  const GIMG_Rendering_Intent intents[] = {GIMG_INTENT_PERCEPTUAL,
      GIMG_INTENT_RELATIVE_COLORIMETRIC, GIMG_INTENT_SATURATION,
      GIMG_INTENT_ABSOLUTE_COLORIMETRIC};
  for (auto intent : intents) {
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(save_as(stating(GIMG_PRIMARIES_SRGB, GIMG_TRANSFER_GAMMA, 2.2,
                            &GIMG_PIXEL_RGBA8, intent),
        "jpeg", jpeg));
    std::vector<uint8_t> profile = profile_in(jpeg);
    ASSERT_GE(profile.size(), 128u);
    EXPECT_EQ(be32(profile.data() + 64), (uint32_t)intent)
        << "ICC.1:2001-04 section 6.1.11 puts the intent at byte 64";
  }
}

/**
 * The policies that state no color space must not get one built for them.
 *
 * A synthesized profile is a color statement, so it follows the same rule the
 * BMP writer follows: GIMG_META_DROP_ALL states nothing, and
 * GIMG_META_KEEP_RAW_ONLY writes only what the file arrived with - and a
 * model on the raster is not that. Every other policy keeps it, because a
 * color space describes what the samples mean rather than annotating them.
 */
TEST(JpegSynthesizedIcc, ThePolicyDecidesWhetherOneIsBuilt) {
  struct {
    GIMG_Meta_Policy policy;
    const char * name;
    bool expect_profile;
  } const cases[] = {
      {GIMG_META_PRESERVE_ALL, "PRESERVE_ALL", true},
      {GIMG_META_STRIP_GPS, "STRIP_GPS", true},
      {GIMG_META_NORMALIZE_EXIF, "NORMALIZE_EXIF", true},
      {GIMG_META_KEEP_COMMON_ONLY, "KEEP_COMMON_ONLY", true},
      {GIMG_META_DROP_ALL, "DROP_ALL", false},
      {GIMG_META_KEEP_RAW_ONLY, "KEEP_RAW_ONLY", false},
  };
  for (const auto & c : cases) {
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(save_as(
        stating(GIMG_PRIMARIES_ADOBE_RGB, GIMG_TRANSFER_GAMMA, 2.2), "jpeg",
        jpeg, c.policy))
        << c.name;
    EXPECT_EQ(!profile_in(jpeg).empty(), c.expect_profile) << c.name;
  }
}

/**
 * The documented case, end to end: a calibrated BMP becomes a tagged JPEG.
 *
 * A BMP states its color space in a V4 header's endpoints and gamma fields,
 * carrying no ICC profile at all. That statement used to survive a save as
 * BMP and, since cHRM, as PNG, but died on the way into a JPEG. This walks
 * the whole road - through the BMP writer, the BMP color reader and the JPEG
 * writer - rather than handing the JPEG writer a raster made for it.
 */
TEST(JpegSynthesizedIcc, ACalibratedBmpReachesAJpegStillSayingSo) {
  std::vector<uint8_t> bmp;
  ASSERT_TRUE(save_as(
      stating(GIMG_PRIMARIES_ADOBE_RGB, GIMG_TRANSFER_GAMMA, 2.2), "bmp",
      bmp));

  // What the BMP kept: the model, and deliberately no profile.
  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bmp.data(), bmp.size(), &in), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  const GIMG_Color_Info * ci = gimg_raster_color_info_const(raster);
  ASSERT_NE(ci, nullptr);
  EXPECT_EQ(ci->primaries, GIMG_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(ci->transfer, GIMG_TRANSFER_GAMMA);
  EXPECT_NEAR(ci->gamma_value, 2.2, 0.01);
  EXPECT_EQ(ci->icc_size, 0u) << "a V4 header states a model without a profile";
  gimg_raster_destroy(raster);

  // Saving that document as a JPEG must not lose it. No raster is attached:
  // the writer decodes for itself, which is the ordinary conversion case.
  std::vector<uint8_t> jpeg;
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &bytes, &size);
  jpeg.assign(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);

  std::vector<uint8_t> profile = profile_in(jpeg);
  ASSERT_GE(profile.size(), 128u)
      << "the gamut the BMP named must reach the JPEG as a profile";
  EXPECT_NEAR(xyz_tag(profile, "gXYZ").y, 0.62567, 0.0001)
      << "and must still be Adobe RGB when it gets there";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
