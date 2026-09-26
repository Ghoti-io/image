/**
 * @file
 *
 * Every TIFF fixture, read by libtiff as well as by this library.
 *
 * The TIFF codec's other tests assert properties - that both byte orders
 * agree, that tiles and strips agree, that a gradient comes back as the
 * gradient that was written. Those are worth having and they are all
 * self-consistency: a decoder that read every file through the same wrong
 * idea would satisfy every one of them. This file is the outside opinion.
 *
 * libtiff is the reference implementation of TIFF and the one every other
 * reader was written against, which makes it the strongest oracle available
 * for this format and *not* a second reading of the specification. Where the
 * document is silent, or where files in the wild disagree with it, libtiff
 * has policies; `documentation/formats/tiff.md` records one this codec does
 * not share. A disagreement here is a question to triage and not a verdict on
 * either side, so the sweep names the files rather than only counting them.
 *
 * The comparison is in RGBA8 because that is what libtiff's RGBA interface
 * returns; a grayscale image this library hands back as GRAY8 is widened the
 * obvious way before comparing, which is what libtiff does internally too.
 *
 * **One conversion stands between the two, and it is a contract and not a
 * tolerance.** libtiff's RGBA raster carries *associated* alpha - its own
 * header names the table that does it, `UaToAa`, "Unassociated alpha to
 * associated alpha conversion LUT" - so a file storing unassociated samples
 * comes back premultiplied. This library's RGBA8 is unassociated, following
 * PNG. Comparing them means putting one into the other's space, and the
 * direction chosen is to premultiply ours, because that is the direction with
 * no division in it and so no case where the two disagree only about
 * rounding. It is applied to every pixel of every file identically, which is
 * what makes it a change of units rather than an exception sized to pass a
 * known failure. Alpha itself is compared unconverted, because nothing
 * transforms it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../../oracle_gate.h"

namespace {

/** One decoded image, as RGBA8 with the stride taken out. */
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
  bool ok = false;
};

std::string data_dir() { return std::string(GIMG_TEST_DATA_TIFF); }
std::string out_dir() { return std::string(GIMG_TEST_OUT_TIFF); }
/** The libtiff sample set, copied out of the pinned image by
 * `make oracle-tools`. Gitignored; the pin is the tarball digest. */
std::string corpus_dir() {
  return std::string(GIMG_TEST_DATA_TIFF) + "/../tiff-corpus";
}

bool looks_like_tiff_name(const std::string & n) {
  return (n.size() > 4u && n.compare(n.size() - 4u, 4u, ".tif") == 0) ||
      (n.size() > 5u && n.compare(n.size() - 5u, 5u, ".tiff") == 0);
}

/** Every .tif under `dir`, one level of subdirectory included - the sample
 * set keeps its bit-depth files in `depth/`. */
void collect(const std::string & dir, const std::string & prefix,
    std::vector<std::string> * out, bool recurse) {
  DIR * d = opendir(dir.c_str());
  if (!d) { return; }
  while (struct dirent * e = readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == "..") { continue; }
    if (looks_like_tiff_name(n)) {
      out->push_back(prefix + n);
    }
    else if (recurse) {
      collect(dir + "/" + n, prefix + n + "/", out, false);
    }
  }
  closedir(d);
}

std::vector<std::string> fixtures() {
  std::vector<std::string> names;
  collect(data_dir(), "", &names, false);
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> corpus() {
  std::vector<std::string> names;
  collect(corpus_dir(), "", &names, true);
  std::sort(names.begin(), names.end());
  return names;
}

/** What this library makes of a file, widened to RGBA8. */
Image ours(const std::string & dir, const std::string & name,
    bool narrow_by_high_byte) {
  Image img;
  std::ifstream f(dir + "/" + name, std::ios::binary);
  const std::vector<uint8_t> bytes(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty()) { return img; }
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
    return img;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) == GIMG_OK && doc) {
    GIMG_Raster * raster = nullptr;
    if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) == GIMG_OK &&
        raster) {
      img.width = gimg_raster_width(raster);
      img.height = gimg_raster_height(raster);
      const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
      const size_t bpp = gimg_raster_bytes_per_pixel(fmt);
      // CMYK8 and RGBA8 are both four bytes, so the model rather than the
      // width is what says which. libtiff's RGBA reader converts separated
      // images to RGB; this codec hands back CMYK, so the comparison does
      // the conversion - with libtiff's own formula, measured below.
      const bool cmyk = fmt && fmt->channel_model == GIMG_CHANNEL_CMYK;
      const size_t stride = gimg_raster_stride_bytes(raster);
      const uint8_t * p = (const uint8_t *)gimg_raster_pixels(raster);
      img.rgba.resize((size_t)img.width * img.height * 4u);
      for (uint32_t y = 0; y < img.height; y++) {
        for (uint32_t x = 0; x < img.width; x++) {
          const uint8_t * src = p + ((size_t)y * stride) + ((size_t)x * bpp);
          uint8_t * dst = img.rgba.data() +
              (((size_t)y * img.width + x) * 4u);
          // A 16-bit raster is narrowed to compare, and **libtiff narrows
          // sixteen bits three different ways depending on which of its
          // paths the file takes**. All three are measured, not assumed:
          //
          //   ColorMap entry   high byte   3139 of 3139 on flower-palette-08
          //   greyscale sample high byte   3000 of 3000 on flower-minisblack-16
          //   RGB sample       rounded     9000 of 9000 on flower-rgb-contig-16
          //
          // The codec follows libtiff only where the narrowing is forced -
          // a ColorMap narrowed on the way into an 8-bit raster - and keeps
          // full precision everywhere else, so a 16-bit file comes back as
          // GRAY16 or RGBA16 and the caller decides. This comparison is
          // therefore where the three rules have to be reproduced, and the
          // caller passes which one applies.
          const uint16_t * wide = (const uint16_t *)(const void *)src;
          auto narrow = [narrow_by_high_byte](uint16_t v) {
            return narrow_by_high_byte
                ? (uint8_t)(v >> 8)
                : (uint8_t)(((uint32_t)v * 255u + 32767u) / 65535u);
          };
          if (cmyk) {
            // libtiff's tif_getimage.c: k = 255 - K, then each channel is
            // (k * (255 - ink)) / 255, truncating. Reproduced rather than
            // approximated, because an "about right" conversion here would
            // absorb a real inversion or channel-order defect.
            const unsigned kk = bpp == 4u
                ? 255u - src[3]
                : 255u - (unsigned)(wide[3] >> 8);
            for (size_t c = 0; c < 3u; c++) {
              const unsigned ink = bpp == 4u
                  ? src[c]
                  : (unsigned)(wide[c] >> 8);
              dst[c] = (uint8_t)((kk * (255u - ink)) / 255u);
            }
            dst[3] = 255u;
            continue;
          }
          switch (bpp) {
          case 1u:
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = 255u;
            break;
          case 2u:
            dst[0] = dst[1] = dst[2] = narrow(wide[0]);
            dst[3] = 255u;
            break;
          case 8u:
            for (size_t k = 0; k < 4u; k++) {
              dst[k] = narrow(wide[k]);
            }
            break;
          default:
            std::memcpy(dst, src, 4u);
            break;
          }
        }
      }
      img.ok = true;
    }
    if (raster) { gimg_raster_destroy(raster); }
  }
  if (doc) { gimg_doc_destroy(doc); }
  gimg_stream_destroy(s);
  return img;
}

/** What libtiff said about one file: its fields, and whether it decoded. */
struct Reference {
  bool ok = false;
  unsigned photometric = 0xFFFFu;
  unsigned compression = 1u;
  unsigned bps = 0u;
  unsigned spp = 0u;
  unsigned planar = 1u;
  bool tiled = false;
};

std::string flatten(const std::string & name) {
  std::string out = name;
  for (char & c : out) {
    if (c == '/') { c = '_'; }
  }
  return out;
}

/**
 * Ask libtiff about every file in one container run.
 *
 * A container start costs more than a decode, and the sample set is
 * sixty-one files: asked one at a time this took twenty-six seconds, of which
 * almost all was engine startup. The tool's --batch mode reads the paths on
 * stdin and writes one raster per file, so the whole sweep is one run.
 */
std::map<std::string, Reference> ask_libtiff(const std::string & dir,
    const std::vector<std::string> & names) {
  std::map<std::string, Reference> out;
  const std::string root = oracle_gate::repo_root();
  if (root.empty()) { return out; }
  if (std::system(("mkdir -p \"" + out_dir() + "\"").c_str()) != 0) {
    return out;
  }

  const std::string list = out_dir() + "/batch.list";
  {
    std::ofstream f(list);
    for (const std::string & n : names) { f << n << "\n"; }
  }
  const std::string answers = out_dir() + "/batch.answers";
  const std::string cmd = "\"" + root + "/tools/oracle/oracle-exec\"" +
      " --scratch \"" + out_dir() + "\" libtiff -- \"" + root +
      "/tests/tools/tiff-oracle/build/dump_tiff_pixels_libtiff\" --batch \"" +
      dir + "\" \"" + out_dir() + "\" < \"" + list + "\" > \"" +
      answers + "\" 2>/dev/null";
  if (std::system(cmd.c_str()) != 0) { return out; }

  std::ifstream f(answers);
  std::string line;
  while (std::getline(f, line)) {
    // name \t status=... \t key=value \t ...
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= line.size()) {
      const size_t tab = line.find('\t', at);
      parts.push_back(line.substr(at, tab == std::string::npos
                  ? std::string::npos
                  : tab - at));
      if (tab == std::string::npos) { break; }
      at = tab + 1u;
    }
    if (parts.size() < 2u) { continue; }
    Reference ref;
    for (size_t i = 1; i < parts.size(); i++) {
      const size_t eq = parts[i].find('=');
      if (eq == std::string::npos) { continue; }
      const std::string key = parts[i].substr(0, eq);
      const std::string val = parts[i].substr(eq + 1u);
      if (key == "status") { ref.ok = (val == "ok"); }
      else if (key == "photometric") { ref.photometric = (unsigned)std::stoul(val); }
      else if (key == "compression") { ref.compression = (unsigned)std::stoul(val); }
      else if (key == "bps") { ref.bps = (unsigned)std::stoul(val); }
      else if (key == "spp") { ref.spp = (unsigned)std::stoul(val); }
      else if (key == "planar") { ref.planar = (unsigned)std::stoul(val); }
      else if (key == "tiled") { ref.tiled = (val != "0"); }
    }
    out[parts[0]] = ref;
  }
  return out;
}

/** The raster libtiff left for one file, if it left one. */
Image reference_pixels(const std::string & name) {
  Image img;
  std::ifstream f(out_dir() + "/" + flatten(name) + ".raw", std::ios::binary);
  const std::vector<uint8_t> blob(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (blob.size() < 12u || std::memcmp(blob.data(), "TIFO", 4) != 0) {
    return img;
  }
  auto u32 = [&](size_t at) {
    return (uint32_t)blob[at] | ((uint32_t)blob[at + 1] << 8) |
        ((uint32_t)blob[at + 2] << 16) | ((uint32_t)blob[at + 3] << 24);
  };
  img.width = u32(4);
  img.height = u32(8);
  const size_t want = (size_t)img.width * img.height * 4u;
  if (blob.size() < 12u + want) { return img; }
  img.rgba.assign(blob.begin() + 12, blob.begin() + 12 + (long)want);
  img.ok = true;
  return img;
}

} // namespace

ORACLE_SENTINEL(TiffOracle, libtiff)

namespace {

/** libtiff's one-line description of a file, for the lists printed below. */
std::string describe(const Reference & r) {
  if (r.photometric == 0xFFFFu && r.bps == 0u) {
    return "libtiff would not open it";
  }
  return "photometric " + std::to_string(r.photometric) + ", " +
      std::to_string(r.bps) + " bits x " + std::to_string(r.spp) +
      ", compression " + std::to_string(r.compression) +
      (r.planar == 2u ? ", planar" : "") + (r.tiled ? ", tiled" : "");
}

struct Tally {
  long agreed = 0;
  long differed = 0;
  long both_refused = 0;
  long only_we_read = 0;
  long only_they_read = 0;
  std::vector<std::string> disagreements;
  std::vector<std::string> we_alone;
  std::vector<std::string> they_alone;
  std::vector<std::string> neither;
  long rounding_samples = 0;
  long rounding_files = 0;
};

/** Compare one directory's files, sample for sample, in libtiff's space. */
void sweep(const std::string & dir, const std::vector<std::string> & names,
    Tally * t) {
  const std::map<std::string, Reference> refs = ask_libtiff(dir, names);
  for (const std::string & name : names) {
    const auto found = refs.find(name);
    const Reference ref = (found == refs.end()) ? Reference() : found->second;
    // Greyscale is the path where libtiff keeps the high byte; see ours().
    const bool gray = ref.photometric == 0u || ref.photometric == 1u;
    const Image a = ours(dir, name, gray);
    const Image b = ref.ok ? reference_pixels(name) : Image();
    if (!a.ok && !b.ok) {
      t->both_refused++;
      t->neither.push_back(name + " (" + describe(ref) + ")");
      continue;
    }
    if (a.ok && !b.ok) {
      t->only_we_read++;
      t->we_alone.push_back(name + " (" + describe(ref) + ")");
      continue;
    }
    if (!a.ok && b.ok) {
      t->only_they_read++;
      t->they_alone.push_back(name + " (" + describe(ref) + ")");
      continue;
    }
    if (a.width != b.width || a.height != b.height) {
      t->differed++;
      t->disagreements.push_back(name + ": geometry, ours " +
          std::to_string(a.width) + "x" + std::to_string(a.height) +
          ", libtiff " + std::to_string(b.width) + "x" +
          std::to_string(b.height));
      continue;
    }
    size_t first_bad = (size_t)-1;
    long bad_samples = 0;
    long rounding_samples = 0;
    const bool palette = ref.photometric == 3u;
    for (size_t i = 0; i < a.rgba.size(); i += 4u) {
      const unsigned alpha = a.rgba[i + 3u];
      for (size_t k = 0; k < 4u; k++) {
        // Alpha passes straight through; colour is premultiplied into
        // libtiff's space first. See the note at the top of this file.
        const unsigned mine = (k == 3u)
            ? alpha
            : ((unsigned)a.rgba[i + k] * alpha + 127u) / 255u;
        const unsigned theirs = b.rgba[i + k];
        if (mine == theirs) { continue; }
        // No tolerance, deliberately. There was one here for a palette
        // rounding difference, and measuring it turned it into a defect
        // instead: libtiff narrows a ColorMap with `v >> 8` and this codec
        // was rescaling, so the two disagreed by one on about half the
        // entries of a real map. The codec matches now and the comparison is
        // exact everywhere, which is the only setting in which a real
        // one-sample defect cannot hide.
        (void)palette;
        bad_samples++;
        if (first_bad == (size_t)-1) { first_bad = i + k; }
      }
    }
    t->rounding_samples += rounding_samples;
    if (rounding_samples > 0) { t->rounding_files++; }
    if (bad_samples == 0) {
      t->agreed++;
    }
    else {
      t->differed++;
      t->disagreements.push_back(name + ": " + std::to_string(bad_samples) +
          " of " + std::to_string(a.rgba.size()) +
          " samples, first at pixel " + std::to_string(first_bad / 4u) +
          " channel " + std::to_string(first_bad % 4u) + ", ours " +
          std::to_string((int)a.rgba[first_bad]) + " (alpha " +
          std::to_string((int)a.rgba[(first_bad & ~(size_t)3u) + 3u]) +
          ") libtiff " + std::to_string((int)b.rgba[first_bad]));
    }
  }
}

void report(const char * label, const Tally & t) {
  std::printf("  %s: %ld agreed, %ld differed, %ld both refused, %ld only we "
              "read, %ld only libtiff read\n",
      label, t.agreed, t.differed, t.both_refused, t.only_we_read,
      t.only_they_read);
  // What libtiff reads and this codec does not is the to-do list, so it is
  // printed rather than counted. The reverse would be a finding: a file this
  // library reads and the reference implementation will not is either not a
  // TIFF or a place where we are being too generous.
  for (const std::string & n : t.they_alone) {
    std::printf("    libtiff reads and we do not: %s\n", n.c_str());
  }
  for (const std::string & n : t.we_alone) {
    std::printf("    we read and libtiff does not: %s\n", n.c_str());
  }
  for (const std::string & n : t.neither) {
    std::printf("    neither reads: %s\n", n.c_str());
  }
  if (t.rounding_files > 0) {
    std::printf("    %ld palette samples in %ld files differ by the "
                "documented rounding of one\n",
        t.rounding_samples, t.rounding_files);
  }
}

} // namespace

TEST(TiffOracle, EveryFixtureLibtiffReadsIsReadTheSameWay) {
  if (!oracle_gate::reachable("libtiff")) {
    GTEST_SKIP() << "the sentinel above has already failed the run";
  }
  const std::vector<std::string> names = fixtures();
  ASSERT_FALSE(names.empty()) << "no fixtures under " << data_dir();
  std::printf("  %s\n", oracle_gate::provenance("libtiff").c_str());

  Tally t;
  sweep(data_dir(), names, &t);
  report("fixtures", t);

  // The alarm on the sweep itself. A comparison that reached no file agrees
  // with everything, and the count is the only thing that separates "libtiff
  // read twelve files and said the same as us" from "libtiff read none".
  EXPECT_GT(t.agreed + t.differed, 8)
      << "only " << (t.agreed + t.differed)
      << " files were compared at all, so this sweep is not seeing the corpus";
  for (const std::string & line : t.disagreements) {
    ADD_FAILURE() << line;
  }
}

/**
 * The same comparison over files this library did not write.
 *
 * tests/data/tiff/ is a generator's output, and a generator writes what its
 * author had already understood: every fixture there covers a case somebody
 * thought of. The libtiff sample set is 76 files written by other encoders
 * against a format none of us wrote, which is the only kind of corpus that
 * can surprise a decoder.
 *
 * **It is not required to be fully read**, and that is the point of running
 * it now rather than when the codec is finished. What it must do is never
 * *disagree*: a file this codec reads at all it must read the same way
 * libtiff does. The count it cannot read is printed every run and is the work
 * queue.
 */
TEST(TiffOracle, NothingInTheLibtiffSampleSetIsReadDifferently) {
  if (!oracle_gate::reachable("libtiff")) {
    GTEST_SKIP() << "the sentinel above has already failed the run";
  }
  const std::vector<std::string> names = corpus();
  ASSERT_FALSE(names.empty())
      << "the libtiff sample set is not in " << corpus_dir()
      << ".\nRun `make oracle-tools`, which copies it out of the pinned "
         "image. It is gitignored on purpose: the pin is the tarball digest "
         "in tools/oracle/VERSIONS, not these bytes.";

  Tally t;
  sweep(corpus_dir(), names, &t);
  report("libtiffpic", t);
  std::printf("  %s\n", oracle_gate::provenance("libtiffpic").c_str());

  // The see-alarm. libtiff's own RGBA reader refuses the 6-, 10-, 12-, 14-,
  // 24- and 32-bit files in this set, so the number of images either reader
  // can open is well under sixty-one; what this asserts is that the sweep is
  // looking at the corpus at all, and it rises as the codec grows.
  EXPECT_GT(t.agreed + t.differed + t.only_they_read, 30)
      << "the sample set has 61 images and this run saw "
      << (t.agreed + t.differed + t.only_they_read);
  for (const std::string & line : t.disagreements) {
    ADD_FAILURE() << line;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
