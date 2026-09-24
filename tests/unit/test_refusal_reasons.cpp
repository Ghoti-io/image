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

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
