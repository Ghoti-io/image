/**
 * @file
 *
 * Replay of the committed fuzz corpus.
 *
 * The fuzzers run by hand; this runs on every `make test`.  Each file under
 * tests/fuzz/corpus/<format>_load is put through what its harness does -
 * load, then decode every item - so an input that once crashed and was kept
 * as a seed is a regression test from then on, under whatever sanitizer the
 * suite is built with.  It asserts nothing about the results: these inputs
 * are mostly invalid, and any GIMG_Result is a legal answer.  What it asserts
 * is that each corpus was found and was not empty, so a moved directory
 * cannot turn this into a test of nothing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "../fuzz/fuzz_limits.h"

namespace {

bool read_all(const std::string & path, std::vector<uint8_t> & out) {
  FILE * f = fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  out.clear();
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  fclose(f);
  return true;
}

/** One seed under one set of limits, as a harness would run it. */
void replay_once(const std::vector<uint8_t> & data, int arm) {
  fuzz_arm_ref() = arm;
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(data.data(), data.size(), &s) != GIMG_OK) {
    return;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, fuzz_load_options(), nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK || !doc) {
    return;
  }
  const size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    (void)gimg_item_decode(gimg_doc_item(doc, i), fuzz_decode_options(), &raster);
    if (raster) {
      gimg_raster_destroy(raster);
    }
  }
  gimg_doc_destroy(doc);
}

/** Every seed under all three arms the harnesses choose between: the
 * four-megapixel cap, the library's defaults, and a small memory cap. */
void replay(const std::vector<uint8_t> & data) {
  if (data.empty()) {
    return;
  }
  for (int arm : {FUZZ_ARM_CAPPED, FUZZ_ARM_DEFAULTS, FUZZ_ARM_SMALL_MEMORY}) {
    replay_once(data, arm);
  }
}

TEST(FuzzCorpus, EveryCommittedLoadSeedReplaysWithoutAFault) {
  const char * formats[] = {"bmp", "gif", "ico", "jpeg", "png", "tiff",
      "webp"};
  for (const char * fmt : formats) {
    const std::string dir = std::string(GIMG_TEST_DATA_ROOT) +
        "/../fuzz/corpus/" + fmt + "_load";
    DIR * d = opendir(dir.c_str());
    ASSERT_NE(d, nullptr) << dir;
    size_t files = 0;
    while (struct dirent * e = readdir(d)) {
      if (e->d_name[0] == '.') {
        continue;
      }
      std::vector<uint8_t> data;
      if (!read_all(dir + "/" + e->d_name, data)) {
        continue;
      }
      SCOPED_TRACE(dir + "/" + e->d_name);
      replay(data);
      files++;
    }
    closedir(d);
    EXPECT_GE(files, 4u) << fmt << ": the committed seeds were not found";
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
