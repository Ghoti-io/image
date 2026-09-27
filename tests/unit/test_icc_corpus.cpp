/**
 * @file
 *
 * The real-ICC corpus: every fixture carrying one, and what happens to it.
 *
 * Until tests/data/icc/generate.py existed, no fixture in this repository
 * carried a profile a CMM could parse.  The three that claimed to were
 * placeholders - one of them the eleven bytes "minimal_icc" - written to
 * assert that an opaque blob survives, which they do correctly and still do.
 * What they could not do is tell a colour transform from one that does
 * nothing, because neither changes anything on a corpus carrying no colour.
 *
 * This library performs no transform, so what it can be held to is carriage:
 * a profile that went in comes out byte for byte, through a decode, through
 * a save, and through a save into a *different* format.  That is what these
 * tests assert, and it is the property a colour engine would later be built
 * on - an engine cannot be correct about a profile the container lost.
 *
 * The profiles themselves are checked elsewhere and by something else:
 * tests/data/icc/verify_icc.py parses each one with littleCMS through the
 * pinned Pillow image, cross-checks the computed colorants against the
 * published figures in src/color/icc_synth.c, and requires swap_rg.icc to
 * actually exchange red and green.  Nothing here re-does that; a C++ test
 * asserting the library's own bytes against themselves would prove nothing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

#include "../oracle_gate.h"

namespace {

std::vector<uint8_t> slurp(const std::string & path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string data_path(const std::string & rel) {
  return oracle_gate::repo_root() + "/tests/data/" + rel;
}

/** The profile a fixture's first item decodes to, or empty. */
std::vector<uint8_t> profile_of(const std::vector<uint8_t> & bytes) {
  std::vector<uint8_t> out;
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
    return out;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) == GIMG_OK && doc) {
    GIMG_Raster * raster = nullptr;
    if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) == GIMG_OK &&
        raster) {
      const GIMG_Color_Info * ci = gimg_raster_color_info_const(raster);
      if (ci && ci->icc_bytes && ci->icc_size > 0) {
        const uint8_t * p = (const uint8_t *)ci->icc_bytes;
        out.assign(p, p + ci->icc_size);
      }
      gimg_raster_destroy(raster);
    }
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return out;
}

/** @p bytes re-saved as @p format, or empty when the codec refused. */
std::vector<uint8_t> resave(
    const std::vector<uint8_t> & bytes, const char * format) {
  std::vector<uint8_t> out;
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return out;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(in, nullptr, nullptr, &doc) == GIMG_OK && doc) {
    GIMG_Stream * os = nullptr;
    if (gimg_stream_create_memory_output(&os) == GIMG_OK) {
      GIMG_Save_Report report;
      std::memset(&report, 0, sizeof(report));
      if (gimg_doc_save(doc, os, format, nullptr, &report) == GIMG_OK) {
        const void * buf = nullptr;
        size_t size = 0;
        gimg_stream_output_buffer(os, &buf, &size);
        const uint8_t * p = (const uint8_t *)buf;
        out.assign(p, p + size);
      }
      gimg_stream_destroy(os);
    }
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(in);
  return out;
}

struct Carrier {
  const char * fixture; ///< Relative to tests/data.
  const char * profile; ///< The .icc it should be carrying, verbatim.
};

const Carrier kCarriers[] = {
    {"png/png_icc_swap_rg.png", "swap_rg.icc"},
    {"png/png_icc_adobergb.png", "adobergb_g22.icc"},
    {"png/png_icc_sampled_trc.png", "srgb_sampled_trc.icc"},
    {"jpeg/jpeg_icc_swap_rg.jpg", "swap_rg.icc"},
    {"tiff/tiff_icc_swap_rg.tif", "swap_rg.icc"},
    {"bmp/bmp_4x4_v5_icc_swap_rg.bmp", "swap_rg.icc"},
};

} // namespace

// Every format that can carry a profile must hand back the bytes that went
// in.  Not "a profile of the right length" and not "something that parses" -
// the same bytes, because a profile altered in transit describes a colour
// space the file did not name.
TEST(IccCorpus, EveryCarrierDecodesToTheProfileOnDisk) {
  for (const Carrier & c : kCarriers) {
    SCOPED_TRACE(c.fixture);
    std::vector<uint8_t> want =
        slurp(data_path(std::string("icc/") + c.profile));
    ASSERT_FALSE(want.empty())
        << "run tests/data/icc/generate.py";
    std::vector<uint8_t> fixture = slurp(data_path(c.fixture));
    ASSERT_FALSE(fixture.empty()) << "run the format's generate.py";

    std::vector<uint8_t> got = profile_of(fixture);
    ASSERT_EQ(got.size(), want.size()) << "profile size changed in carriage";
    EXPECT_EQ(std::memcmp(got.data(), want.data(), want.size()), 0)
        << "the bytes differ, so the colour this file names is not the "
           "colour it was given";
  }
}

// A save must not lose it either, and the interesting direction is into a
// *different* container: the profile then passes through one codec's reader
// and another's writer, which is where a length field or a segment boundary
// gets it wrong.  JPEG is the one that splits a profile across APP2 segments
// when it is long enough, which srgb_sampled_trc.icc is.
TEST(IccCorpus, AProfileSurvivesACrossFormatSave) {
  struct Case {
    const char * fixture;
    const char * profile;
    const char * to;
  };
  const Case cases[] = {
      {"png/png_icc_swap_rg.png", "swap_rg.icc", "jpeg"},
      {"png/png_icc_swap_rg.png", "swap_rg.icc", "tiff"},
      {"png/png_icc_swap_rg.png", "swap_rg.icc", "bmp"},
      {"jpeg/jpeg_icc_swap_rg.jpg", "swap_rg.icc", "png"},
      {"tiff/tiff_icc_swap_rg.tif", "swap_rg.icc", "png"},
      {"bmp/bmp_4x4_v5_icc_swap_rg.bmp", "swap_rg.icc", "png"},
      // The long one, which JPEG has to split and reassemble.
      {"png/png_icc_sampled_trc.png", "srgb_sampled_trc.icc", "jpeg"},
      {"png/png_icc_sampled_trc.png", "srgb_sampled_trc.icc", "tiff"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.fixture) + " -> " + c.to);
    std::vector<uint8_t> want =
        slurp(data_path(std::string("icc/") + c.profile));
    ASSERT_FALSE(want.empty()) << "run tests/data/icc/generate.py";
    std::vector<uint8_t> fixture = slurp(data_path(c.fixture));
    ASSERT_FALSE(fixture.empty());

    std::vector<uint8_t> saved = resave(fixture, c.to);
    ASSERT_FALSE(saved.empty()) << "the save failed";
    std::vector<uint8_t> got = profile_of(saved);
    ASSERT_EQ(got.size(), want.size())
        << "the profile did not survive the crossing at its original length";
    EXPECT_EQ(std::memcmp(got.data(), want.data(), want.size()), 0);
  }
}

// The placeholders stay, and this says why: an eleven-byte "profile" is a
// test of the carrying path at a size no real profile has, and deleting it
// because a real corpus now exists would lose that.  The two kinds of
// fixture answer different questions.
TEST(IccCorpus, ThePlaceholderFixturesStillCarryTheirOddSizes) {
  const struct {
    const char * fixture;
    size_t size;
  } stubs[] = {
      {"png/png_iccp.png", 11},
      {"jpeg/jpeg_with_icc.jpg", 128},
  };
  for (const auto & s : stubs) {
    SCOPED_TRACE(s.fixture);
    std::vector<uint8_t> got = profile_of(slurp(data_path(s.fixture)));
    EXPECT_EQ(got.size(), s.size)
        << "a blob of an awkward size is still carried unchanged";
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
