/**
 * @file
 *
 * A characterisation sweep: what colour every committed fixture decodes to.
 *
 * The redesign that made GIMG_Color_Info store a gamut exactly could have
 * changed what any file already in the tree means, and the corpus is far too
 * large to check by reading.  So this walks every fixture, records the colour
 * interpretation as one line, and compares the whole set against a table
 * committed beside it.  A change to any decoder's colour handling then has to
 * be agreed to in the diff rather than discovered later.
 *
 * It is a characterisation test and not a correctness one: the table says
 * what the library does, not what it should do.  That is the point - the
 * value is in making a change visible, and a line that looks wrong in the
 * diff is the finding.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

#include "../oracle_gate.h"

namespace {

const char * primaries_name(GIMG_Primaries p) {
  switch (p) {
  case GIMG_PRIMARIES_SRGB:
    return "sRGB";
  case GIMG_PRIMARIES_ADOBE_RGB:
    return "AdobeRGB";
  case GIMG_PRIMARIES_DISPLAY_P3:
    return "DisplayP3";
  case GIMG_PRIMARIES_BT2020:
    return "BT2020";
  case GIMG_PRIMARIES_PROPHOTO:
    return "ProPhoto";
  case GIMG_PRIMARIES_UNKNOWN:
    return "-";
  default:
    return "?";
  }
}

const char * reference_name(GIMG_Reference r) {
  switch (r) {
    case GIMG_REFERENCE_DISPLAY: return "display";
    case GIMG_REFERENCE_SCENE: return "scene";
    case GIMG_REFERENCE_UNKNOWN: return "-";
    default: return "?";
  }
}

const char * scale_name(GIMG_Sample_Scale s) {
  switch (s) {
    case GIMG_SAMPLE_SCALE_RELATIVE: return "relative";
    case GIMG_SAMPLE_SCALE_ABSOLUTE: return "absolute";
    case GIMG_SAMPLE_SCALE_UNKNOWN: return "-";
    default: return "?";
  }
}

const char * transfer_name(GIMG_Transfer t) {
  switch (t) {
  case GIMG_TRANSFER_LINEAR:
    return "linear";
  case GIMG_TRANSFER_SRGB:
    return "sRGB";
  case GIMG_TRANSFER_GAMMA:
    return "gamma";
  case GIMG_TRANSFER_PARAMETRIC:
    return "parametric";
  case GIMG_TRANSFER_BT1886:
    return "BT1886";
  case GIMG_TRANSFER_PQ:
    return "PQ";
  case GIMG_TRANSFER_HLG:
    return "HLG";
  case GIMG_TRANSFER_UNKNOWN:
    return "-";
  default:
    return "?";
  }
}

/** One fixture's colour interpretation, as one comparable line. */
std::string describe(const std::string & rel, const std::vector<uint8_t> & b) {
  std::ostringstream o;
  o << rel << " | ";
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(b.data(), b.size(), &in) != GIMG_OK) {
    o << "stream-failed";
    return o.str();
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(in, nullptr, nullptr, &doc) != GIMG_OK || !doc) {
    o << "load-refused";
    gimg_stream_destroy(in);
    return o.str();
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) != GIMG_OK ||
      !raster) {
    o << "decode-refused";
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
    return o.str();
  }
  const GIMG_Color_Info * ci = gimg_raster_color_info_const(raster);
  if (!ci) {
    o << "no-color-info";
  }
  else {
    o << "gamut=" << (ci->primaries_stated ? "yes" : "no")
      << " white=" << (ci->white_stated ? "yes" : "no") << " named="
      << primaries_name(
             gimg_gamut_identify(&ci->gamut, GIMG_GAMUT_TOLERANCE_DEFAULT))
      << " transfer=" << transfer_name(ci->transfer);
    if (ci->transfer == GIMG_TRANSFER_GAMMA) {
      char g[32];
      (void)std::snprintf(g, sizeof(g), " gamma=%.4f", ci->gamma_value);
      o << g;
    }
    o << " reference=" << reference_name(ci->reference)
      << " scale=" << scale_name(ci->sample_scale);
    if (ci->white_luminance != 0.0) {
      char n[40];
      (void)std::snprintf(
          n, sizeof(n), " white_cdm2=%.1f", ci->white_luminance);
      o << n;
    }
    o << " icc=" << ci->icc_size
      << " linked=" << (ci->icc_linked_path ? "yes" : "no");
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  return o.str();
}

/**
 * Is this path a cache rather than a committed fixture?
 *
 * third_party and the tiff-* directories are gitignored: they are fetched by
 * `make oracle-tools` from a pinned image, so whether they exist depends on
 * the machine.  Sweeping them would make the recorded table machine-specific
 * and fail on a fresh clone for a reason that is not a defect.  The
 * population this test measures is what the repository commits.
 */
bool is_generated(const std::string & rel) {
  static const char * const skip[] = {"tests/data/tiff-corpus/",
      "tests/data/tiff-fax/", "tests/data/tiff-depth/", "tests/data/out/"};
  for (const char * s : skip) {
    if (rel.rfind(s, 0) == 0) {
      return true;
    }
  }
  return false;
}

bool is_image(const std::filesystem::path & p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  return e == ".png" || e == ".bmp" || e == ".jpg" || e == ".jpeg" ||
      e == ".tif" || e == ".tiff" || e == ".gif";
}

} // namespace

TEST(ColorCorpus, EveryFixtureDecodesToTheRecordedColour) {
  const std::string root = oracle_gate::repo_root();
  ASSERT_FALSE(root.empty()) << "set GIMG_IMAGE_ROOT";

  std::vector<std::string> lines;
  for (const char * sub : {"tests/data"}) {
    std::filesystem::path dir = std::filesystem::path(root) / sub;
    if (!std::filesystem::exists(dir)) {
      continue;
    }
    for (const auto & entry :
        std::filesystem::recursive_directory_iterator(dir)) {
      if (!entry.is_regular_file() || !is_image(entry.path())) {
        continue;
      }
      std::string rel =
          std::filesystem::relative(entry.path(), std::filesystem::path(root))
              .generic_string();
      if (is_generated(rel)) {
        continue;
      }
      std::ifstream f(entry.path(), std::ios::binary);
      if (!f) {
        continue;
      }
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>());
      lines.push_back(describe(rel, bytes));
    }
  }
  std::sort(lines.begin(), lines.end());
  ASSERT_GT(lines.size(), 100u)
      << "the sweep found almost nothing, so it is measuring its own reach "
         "rather than the corpus";

  const std::filesystem::path expected_path =
      std::filesystem::path(root) / "tests/data/color_interpretation.txt";
  std::ifstream exp(expected_path);
  if (!exp) {
    // First run, or the table was deleted: write it so the diff is reviewable
    // rather than failing with nothing to compare against.
    std::ofstream out(expected_path);
    for (const auto & l : lines) {
      out << l << "\n";
    }
    FAIL() << "no recorded table; wrote " << lines.size() << " lines to "
           << expected_path << " - review the diff and commit it";
  }
  std::vector<std::string> want;
  std::string line;
  while (std::getline(exp, line)) {
    if (!line.empty()) {
      want.push_back(line);
    }
  }

  // Report every difference rather than the first: a change to one codec's
  // colour handling moves many files at once, and the shape of the set is
  // what says which codec it was.
  std::vector<std::string> only_now, only_then;
  std::set_difference(lines.begin(), lines.end(), want.begin(), want.end(),
      std::back_inserter(only_now));
  std::set_difference(want.begin(), want.end(), lines.begin(), lines.end(),
      std::back_inserter(only_then));
  EXPECT_TRUE(only_now.empty() && only_then.empty())
      << "the colour a fixture decodes to has changed.\n"
      << "now:\n"
      <<
      [&] {
        std::string s;
        for (const auto & l : only_now) {
          s += "  " + l + "\n";
        }
        return s;
      }()
      << "recorded:\n"
      <<
      [&] {
        std::string s;
        for (const auto & l : only_then) {
          s += "  " + l + "\n";
        }
        return s;
      }()
      << "If the change is intended, delete " << expected_path
      << " and re-run to regenerate it, then review the diff.";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
