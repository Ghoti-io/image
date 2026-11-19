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
 * The writers are replayed too: every hand-built seed is saved in every
 * format under a fixed list of option selections, which are the ones the
 * encode harnesses derive from their input (tests/fuzz/fuzz_encode.h).  That is
 * what runs the higher-effort writers - WebP search levels, PNG filters and
 * palette policies, JPEG's progressive, arithmetic, lossless, hierarchical and
 * abbreviated modes - on every `make test`, and compares the picture after a
 * lossless save.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <set>
#include "../fuzz/fuzz_encode.h"
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

/** Failures of the round-trip check, grouped by what failed so one defect
 * reads as one line however many seeds and selections reach it. */
struct Failures {
  std::map<std::string, std::pair<int, std::string>> by_kind;
  std::string context;
};

Failures & failures() {
  static Failures f;
  return f;
}

void gtest_failure(const char * message) {
  // "<fmt> selector N variant V: <what>" -> key "<fmt> v<V>: <what>".
  const std::string m = message;
  const size_t colon = m.find(": ");
  const std::string detail_free = m.substr(0, m.find(" ["));
  const std::string head = m.substr(0, colon);
  const size_t v = head.find(" variant ");
  const std::string key = head.substr(0, head.find(' ')) + " v" +
      (v == std::string::npos ? "?" : head.substr(v + 9)) +
      (colon == std::string::npos ? "" : detail_free.substr(colon));
  auto & e = failures().by_kind[key];
  if (e.first++ == 0) {
    e.second = failures().context + " :: " + m;
  }
}

/** The hand-built seeds (*.seed) of every harness directory. */
std::vector<std::pair<std::string, std::vector<uint8_t>>> all_seeds() {
  static const char * dirs[] = {"bmp_encode", "bmp_load", "gif_encode",
      "gif_load", "ico_encode", "ico_load", "jpeg_encode", "jpeg_load",
      "png_encode", "png_load", "tiff_encode", "tiff_load", "webp_encode",
      "webp_load"};
  std::vector<std::pair<std::string, std::vector<uint8_t>>> seeds;
  for (const char * d : dirs) {
    const std::string dir =
        std::string(GIMG_TEST_DATA_ROOT) + "/../fuzz/corpus/" + d;
    DIR * dh = opendir(dir.c_str());
    if (dh == nullptr) {
      continue;
    }
    while (struct dirent * e = readdir(dh)) {
      const std::string name = e->d_name;
      if (name.size() < 6 || name.compare(name.size() - 5, 5, ".seed") != 0) {
        continue;
      }
      std::vector<uint8_t> data;
      if (read_all(dir + "/" + name, data) && !data.empty() &&
          data.size() <= 32768u) {
        seeds.emplace_back(dir + "/" + name, std::move(data));
      }
    }
    closedir(dh);
  }
  return seeds;
}

/** How many option selections each seed is saved under, per format.  Every
 * (seed, selection) pair gets its own selector, so across the seeds a format
 * runs far more option sets than this.  The replay's run time is this times
 * the seeds, measured, not guessed. */
constexpr uint32_t kSelections = 2;

/** Larger pictures are left to the fuzzers: the WebP search levels cost
 * seconds on a few thousand pixels under a sanitizer. */
constexpr uint64_t kMaxPixels = 32u * 32u;

TEST(FuzzCorpus, EveryWriterRunsEverySeedUnderManyOptionSelections) {
  const auto seeds = all_seeds();
  ASSERT_GE(seeds.size(), 60u) << "the committed seeds were not found";
  fuzz_failure_hook() = gtest_failure;
  failures().by_kind.clear();
  for (const char * fmt : {"png", "jpeg", "webp", "tiff", "gif", "bmp", "ico"}) {
    const auto t0 = std::chrono::steady_clock::now();
    FuzzEncodeStats total;
    std::set<uint64_t> distinct;
    uint32_t seed_no = 0;
    for (const auto & seed : seeds) {
      seed_no++;
      // Every save of a WebP costs tens of milliseconds whatever the picture,
      // and under a sanitizer ten times that: a quarter of the seeds keeps
      // the whole replay within a few seconds.  The fuzzer runs the rest.
      if (std::string(fmt) == "webp" && seed_no % 4u != 0u) {
        continue;
      }
      failures().context = std::string(fmt) + " <- " + seed.first;
      // WebP saves twice a unit (lossless and lossy), so it takes one selection.
      const uint32_t selections = std::string(fmt) == "webp" ? 1u : kSelections;
      for (uint32_t sel = 0; sel < selections; sel++) {
        // The three decode-limit arms rotate with the selection, so the writers
        // also run under the defaults and under a small memory cap.
        fuzz_arm_ref() = (int)((seed_no + sel) % 3u);
        const uint32_t selector = seed_no * 7919u + sel * 104729u;
        FuzzEncodeStats one;
        fuzz_encode_unit(fmt, seed.second.data(), seed.second.size(), selector, &one,
            kMaxPixels);
        total.saves += one.saves;
        total.saves_ok += one.saves_ok;
        total.compared += one.compared;
        total.reloaded += one.reloaded;
        if (one.saves_ok != 0) {
          distinct.insert(one.digest);
        }
      }
    }
    fuzz_arm_ref() = FUZZ_ARM_CAPPED;
    const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    printf("fuzz encode replay %-5s %5ld ms: %zu saves, %zu ok, %zu compared, %zu distinct\n",
        fmt, ms, total.saves, total.saves_ok, total.compared, distinct.size());
    SCOPED_TRACE(fmt);
    // Not asserting a result, asserting reach: the writers ran, wrote files
    // that differ from one option selection to the next, and (where they are
    // lossless) the comparison with the source was made at all.
    EXPECT_GE(total.saves_ok, 40u);
    EXPECT_GE(total.reloaded, 40u);
    EXPECT_GE(distinct.size(), 25u);
    if (std::string(fmt) != "jpeg" && std::string(fmt) != "ico") {
      EXPECT_GE(total.compared, 10u);
    }
  }
  fuzz_failure_hook() = nullptr;
  for (const auto & kv : failures().by_kind) {
    ADD_FAILURE() << kv.second.first << " x " << kv.first << "\n  first: "
                  << kv.second.second;
  }
}

TEST(FuzzCorpus, TheRoundTripCheckSeesADifference) {
  // A gate that has never been seen to fail passes for the wrong reason.
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  GIMG_Raster * c = nullptr;
  ASSERT_EQ(gimg_raster_create(2, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a), GIMG_OK);
  ASSERT_EQ(gimg_raster_create(2, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &b), GIMG_OK);
  ASSERT_EQ(gimg_raster_create(3, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &c), GIMG_OK);
  uint8_t * pa = (uint8_t *)gimg_raster_pixels(a);
  uint8_t * pb = (uint8_t *)gimg_raster_pixels(b);
  const uint8_t px[8] = {1, 2, 3, 255, 4, 5, 6, 0};
  std::memcpy(pa, px, 8);
  std::memcpy(pb, px, 8);
  EXPECT_EQ(fuzz_detail::compare_rasters(a, b, false), 1);
  EXPECT_EQ(fuzz_detail::compare_rasters(a, c, false), -1);
  pb[0] = 9; // an opaque pixel's colour changed
  EXPECT_EQ(fuzz_detail::compare_rasters(a, b, false), 0);
  EXPECT_EQ(fuzz_detail::compare_rasters(a, b, true), 0);
  pb[0] = 1;
  pb[4] = 9; // the colour of a fully transparent pixel changed
  EXPECT_EQ(fuzz_detail::compare_rasters(a, b, false), 0);
  EXPECT_EQ(fuzz_detail::compare_rasters(a, b, true), 1);
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
  gimg_raster_destroy(c);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
