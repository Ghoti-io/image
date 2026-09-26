/**
 * @file
 *
 * A refused file says which rule it broke.
 *
 * GIMG_ERR_FORMAT covers an IHDR with a colour type of 5, a PLTE on a
 * grayscale image, a second SOF outside a hierarchical sequence, and a file
 * that ends inside a segment it declared the length of. A caller holding a
 * file that will not load has four bits of information and nothing to act on
 * - and this library's own documentation tells them otherwise: "use the
 * result code and **GIMG_Diagnostics**, which load and decode do fill."
 *
 * They did not all fill it. Measured over the fuzz corpus, 8,403 files with a
 * recognised signature: the BMP loader explained every one of its 215
 * refusals and the GIF loader every one of its 97, while the PNG loader
 * explained 51 of 245 and the JPEG loader 1,687 of 1,832. **339 refusals said
 * nothing at all.**
 *
 * This is the gate that keeps it at zero, and it is a total assertion rather
 * than a sample: every file in the corpus, every refusal, a reason. The
 * corpus is the right population because it is made of the inputs that
 * actually reach these paths - a fuzzer's mutations of real files, which is
 * what a malformed image in the wild looks like - and because no fixture set
 * anyone writes by hand covers the same ground.
 *
 * Two things are checked besides:
 *
 *   - **The sweep is not blind.** A wrong path would find no files and report
 *     a clean run, so the refusal count itself is asserted to be large. A
 *     sweep that cannot see returns the same number as one that found nothing
 *     wrong.
 *   - A file that *loads* must not report an error, so a loader that appended
 *     "something went wrong" unconditionally could not pass.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

/** Whether directory entry @p e of @p dir is itself a directory. MinGW's
 * struct dirent has no d_type, so Windows asks stat() instead. */
bool entry_is_directory(const std::string & dir, const struct dirent * e) {
#if defined(_WIN32)
  struct stat st;
  return stat((dir + "/" + e->d_name).c_str(), &st) == 0 &&
      S_ISDIR(st.st_mode);
#else
  (void)dir;
  return e->d_type == DT_DIR;
#endif
}

/** The format a file's first bytes claim, or "" if none of the four. */
const char * signature_of(const std::vector<uint8_t> & b) {
  if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xD8) { return "jpeg"; }
  if (b.size() >= 8 && memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
    return "png";
  }
  if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M') { return "bmp"; }
  if (b.size() >= 3 && memcmp(b.data(), "GIF", 3) == 0) { return "gif"; }
  return nullptr;
}

struct Tally {
  long refused = 0;
  long silent = 0;
  long loaded = 0;
  long loaded_with_error = 0;
  std::string first_silent;
};

void sweep(const std::string & dir, std::map<std::string, Tally> & by_format) {
  DIR * d = opendir(dir.c_str());
  if (!d) { return; }
  while (struct dirent * e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == ".." || entry_is_directory(dir, e)) { continue; }
    std::ifstream f(dir + "/" + name, std::ios::binary | std::ios::ate);
    if (!f) { continue; }
    const std::streamsize size = f.tellg();
    // A 64MB ceiling so an oracle dump left in a data directory cannot turn
    // this into a memory test; nothing that exercises a loader is near it.
    if (size <= 0 || size > (std::streamsize)(64 << 20)) { continue; }
    f.seekg(0);
    std::vector<uint8_t> bytes((size_t)size);
    f.read((char *)bytes.data(), size);
    const char * fmt = signature_of(bytes);
    if (!fmt) { continue; }

    GIMG_Stream * s = nullptr;
    if (gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
      continue;
    }
    GIMG_Diagnostics diag = {};
    GIMG_Doc * doc = nullptr;
    const GIMG_Result r = gimg_doc_load(s, nullptr, &diag, &doc);
    long errors = 0;
    for (size_t i = 0; i < diag.count; i++) {
      if (diag.items[i].severity == GIMG_DIAG_ERROR) { errors++; }
    }
    Tally & t = by_format[fmt];
    if (r != GIMG_OK) {
      t.refused++;
      if (errors == 0) {
        t.silent++;
        if (t.first_silent.empty()) {
          t.first_silent = name + " (returned " + std::to_string((int)r) + ")";
        }
      }
    }
    else {
      t.loaded++;
      if (errors > 0) {
        t.loaded_with_error++;
        if (t.first_silent.empty()) {
          t.first_silent = name + " (loaded, but reported an error)";
        }
      }
    }
    gimg_diagnostics_destroy(&diag);
    if (doc) { gimg_doc_destroy(doc); }
    gimg_stream_destroy(s);
  }
  closedir(d);
}

} // namespace

TEST(RefusalReasons, EveryRefusedFileSaysWhichRuleItBroke) {
  const std::string data_jpeg = GIMG_TEST_DATA_JPEG;
  // The fuzz corpus and the fixture sets, located relative to the one data
  // directory the build names.
  const std::string root = data_jpeg + "/../..";
  const std::string dirs[] = {
      root + "/fuzz/corpus",
      root + "/data/jpeg",
      root + "/data/png",
      root + "/data/bmp",
      root + "/data/gif",
  };
  std::map<std::string, Tally> by_format;
  for (const std::string & dir : dirs) {
    sweep(dir, by_format);
  }

  long total_refused = 0;
  for (const auto & kv : by_format) {
    const Tally & t = kv.second;
    total_refused += t.refused;
    std::printf("  %-5s refused %5ld, loaded %5ld\n", kv.first.c_str(),
        t.refused, t.loaded);
    EXPECT_EQ(t.silent, 0)
        << kv.first << ": " << t.silent << " of " << t.refused
        << " refusals said nothing; first was " << t.first_silent;
    EXPECT_EQ(t.loaded_with_error, 0)
        << kv.first << ": " << t.loaded_with_error
        << " files loaded and reported an error anyway; first was "
        << t.first_silent;
  }

  // The alarm on the sweep itself: a wrong path finds no files and every
  // assertion above passes vacuously.
  ASSERT_GT(total_refused, 500)
      << "only " << total_refused << " refusals were seen, so this sweep is "
         "looking at the wrong place - check that tests/fuzz/corpus is where "
         "it is expected relative to " << data_jpeg;
  EXPECT_EQ(by_format.size(), 4u)
      << "one of the four formats contributed no files at all";
}

namespace {

struct DecodeTally {
  long files = 0;     ///< Files that loaded.
  long items = 0;     ///< Items decoded or refused.
  long decoded = 0;   ///< Items that came back with a raster.
  long refused = 0;   ///< Items refused with a code that names a reason.
  long odd = 0;       ///< Items refused with a code that does not.
  long inconsistent = 0; ///< A non-OK result that still handed back a raster.
  std::string first_odd;
};

/** Load every file in @p dir and decode every item of the ones that load. */
void sweep_decode(const std::string & dir, const GIMG_Limits & limits,
    std::map<std::string, DecodeTally> & by_format) {
  DIR * d = opendir(dir.c_str());
  if (!d) { return; }
  while (struct dirent * e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == ".." || entry_is_directory(dir, e)) { continue; }
    std::ifstream f(dir + "/" + name, std::ios::binary | std::ios::ate);
    if (!f) { continue; }
    const std::streamsize size = f.tellg();
    if (size <= 0 || size > (std::streamsize)(64 << 20)) { continue; }
    f.seekg(0);
    std::vector<uint8_t> bytes((size_t)size);
    if (!f.read((char *)bytes.data(), size)) { continue; }
    const char * sig = signature_of(bytes);
    if (!sig) { continue; }

    GIMG_Stream * in = nullptr;
    if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
      continue;
    }
    GIMG_Load_Options lo = {};
    lo.limits = &limits;
    GIMG_Doc * doc = nullptr;
    if (gimg_doc_load(in, &lo, nullptr, &doc) == GIMG_OK && doc) {
      DecodeTally & t = by_format[sig];
      t.files++;
      GIMG_Decode_Options dopt = {};
      dopt.limits = &limits;
      const size_t n = gimg_doc_item_count(doc);
      for (size_t i = 0; i < n; i++) {
        GIMG_Item * item = gimg_doc_item(doc, i);
        if (!item) { continue; }
        GIMG_Raster * raster = nullptr;
        const GIMG_Result r = gimg_item_decode(item, &dopt, &raster);
        t.items++;
        if (r == GIMG_OK) {
          t.decoded++;
          if (!raster) {
            t.inconsistent++;
            if (t.first_odd.empty()) { t.first_odd = name + " (OK, no raster)"; }
          }
        }
        else {
          // The codes a decoder is allowed to give up with. GIMG_ERR_INTERNAL
          // is not among them: it means the library confused itself, which a
          // file should never be able to arrange.
          const bool named = r == GIMG_ERR_CORRUPT || r == GIMG_ERR_FORMAT ||
              r == GIMG_ERR_UNSUPPORTED || r == GIMG_ERR_LIMIT ||
              r == GIMG_ERR_OOM;
          if (named) { t.refused++; }
          else {
            t.odd++;
            if (t.first_odd.empty()) {
              t.first_odd = name + " (returned " + std::to_string((int)r) + ")";
            }
          }
          if (raster) {
            t.inconsistent++;
            if (t.first_odd.empty()) {
              t.first_odd = name + " (failed and still returned a raster)";
            }
          }
        }
        if (raster) { gimg_raster_destroy(raster); }
      }
    }
    if (doc) { gimg_doc_destroy(doc); }
    gimg_stream_destroy(in);
  }
  closedir(d);
}

} // namespace

/**
 * Every item of every file that loads either decodes or gives a named reason.
 *
 * The sweep above stops at the load, which is where the corpus was pointed
 * when it was written. A load only parses headers, so every entropy decoder,
 * every LZW walker and every row filter in this library sat behind it
 * untouched by the one population that is actually made of malformed files -
 * and those are exactly the paths whose refusal arms no hand-written fixture
 * reaches.
 *
 * What is asserted is not that a file decodes. Most of these are mutations
 * and most of them should fail. It is that a failure is one of the codes a
 * decoder is allowed to end with, that GIMG_ERR_INTERNAL is never one of them
 * - the library confusing itself is not something a file should be able to
 * arrange - and that a failed decode hands back nothing, since a caller who
 * checks the result and then frees the raster on the success path alone
 * cannot be expected to free one that arrived with an error.
 *
 * A pixel cap keeps a header claiming an enormous frame from turning this
 * into a memory test; it is the cap the sweep in test_limits.cpp shows every
 * codec reads.
 */
TEST(RefusalReasons, EveryItemThatLoadsDecodesOrSaysWhyNot) {
  const std::string data_jpeg = GIMG_TEST_DATA_JPEG;
  const std::string root = data_jpeg + "/../..";
  const std::string dirs[] = {
      root + "/fuzz/corpus",
      root + "/data/jpeg",
      root + "/data/png",
      root + "/data/bmp",
      root + "/data/gif",
  };
  GIMG_Limits limits;
  gimg_limits_default(&limits);
  limits.max_decoded_pixels = 4u * 1024u * 1024u;
  limits.max_frame_count = 64u;

  std::map<std::string, DecodeTally> by_format;
  for (const std::string & dir : dirs) {
    sweep_decode(dir, limits, by_format);
  }

  long total_items = 0, total_refused = 0;
  for (const auto & kv : by_format) {
    const DecodeTally & t = kv.second;
    total_items += t.items;
    total_refused += t.refused;
    std::printf("  %-5s %5ld files, %5ld items: %5ld decoded, %5ld refused\n",
        kv.first.c_str(), t.files, t.items, t.decoded, t.refused);
    EXPECT_EQ(t.odd, 0) << kv.first << ": " << t.odd
                        << " decode(s) ended with a code that names no "
                           "reason; first was " << t.first_odd;
    EXPECT_EQ(t.inconsistent, 0)
        << kv.first << ": " << t.inconsistent
        << " decode(s) disagreed with their own result; first was "
        << t.first_odd;
  }

  // The same alarm the load sweep carries: a wrong path finds nothing and
  // every assertion above passes vacuously.
  ASSERT_GT(total_items, 1000)
      << "only " << total_items << " items were decoded, so this sweep is "
         "looking at the wrong place - check that tests/fuzz/corpus is where "
         "it is expected relative to " << data_jpeg;
  EXPECT_GT(total_refused, 0)
      << "not one decode in the whole corpus was refused, which is not what a "
         "corpus of mutations looks like";
  EXPECT_EQ(by_format.size(), 4u)
      << "one of the four formats contributed no file that loads";
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
