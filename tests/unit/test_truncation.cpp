/**
 * @file
 *
 * What the codecs do with a file that stops in the middle.
 *
 * Truncation is the commonest malformed input there is - an interrupted
 * download, a partial write, a file copied while it was still being made -
 * and it is the one shape that reaches a decoder's bounds checks from the
 * outside rather than through a crafted field. The suite had a handful of
 * hand-written cases for particular cut points; the fuzzers cover the rest,
 * but they are separate targets that `make test` does not run.
 *
 * So this cuts every fixture in the tree at a spread of lengths and asks two
 * things of each: that the answer is an answer rather than a crash, and that
 * whatever was allocated on the way is handed back. The second is the one
 * that needs the failing allocator: a truncated file makes a decoder give up
 * partway through, which is exactly where a cleanup path is easiest to get
 * wrong, and a leak there is invisible to a test that only looks at the
 * return code.
 *
 * Cut points are every byte of the first 96 - where the headers are, so this
 * is where a length field gets believed - and then a spread across the rest.
 * Cutting at every byte of every fixture would be some millions of decodes;
 * this is a few tens of thousands and runs in seconds.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

#include "../failing_allocator.h"
#include "../../src/codec/codec_internal.h"
#include "../registry_sweep.h"

namespace {

using gimg_test::Failing;
using gimg_test::init;

bool read_file(const std::string & path, std::vector<uint8_t> & out) {
  FILE * fp = fopen(path.c_str(), "rb");
  if (!fp) { return false; }
  fseek(fp, 0, SEEK_END);
  const long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (n < 0) { fclose(fp); return false; }
  out.resize((size_t)n);
  const bool ok = fread(out.data(), 1u, (size_t)n, fp) == (size_t)n;
  fclose(fp);
  return ok;
}

/** Load and decode @p bytes through @p f, so what it took can be counted. */
GIMG_Result decode_through(const char * codec_name, const uint8_t * bytes,
    size_t len, Failing & f) {
  GIMG_Codec * codec = gimg_codec_by_name(codec_name);
  if (!codec) { return GIMG_ERR_UNSUPPORTED; }
  const GIMG_Allocator * saved = codec->allocator;
  codec->allocator = &f.a;

  GIMG_Stream * in = nullptr;
  GIMG_Result r = gimg_stream_create_memory(bytes, len, &in);
  if (r == GIMG_OK) {
    GIMG_Doc * doc = nullptr;
    r = gimg_doc_load(in, nullptr, nullptr, &doc);
    if (r == GIMG_OK && doc) {
      GIMG_Item * item = gimg_doc_item(doc, 0);
      GIMG_Raster * raster = nullptr;
      r = item ? gimg_item_decode(item, nullptr, &raster) : GIMG_ERR_INTERNAL;
      if (raster) { gimg_raster_destroy(raster); }
    }
    if (doc) { gimg_doc_destroy(doc); }
    gimg_stream_destroy(in);
  }
  codec->allocator = saved;
  return r;
}

/** The lengths to cut at: every header byte, then a spread over the rest. */
std::vector<size_t> cut_points(size_t len) {
  std::vector<size_t> out;
  const size_t head = std::min<size_t>(len, 96u);
  for (size_t i = 1; i <= head; i++) { out.push_back(i); }
  if (len > head) {
    const size_t steps = 48;
    for (size_t k = 1; k <= steps; k++) {
      const size_t at = head + (len - head) * k / steps;
      if (at > head && at < len) { out.push_back(at); }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

} // namespace

TEST(Truncation, EveryFixtureCutShortIsAnsweredAndFreed) {
  const std::vector<gimg_test::SweptCodec> codecs = gimg_test::swept_codecs();
  ASSERT_FALSE(codecs.empty()) << "no codecs registered; nothing to sweep";

  long fixtures = 0, cuts = 0, refused = 0, decoded = 0;
  for (const gimg_test::SweptCodec & d : codecs) {
    const std::vector<std::string> names =
        gimg_test::sweep_image_files_in(d.data_dir);
    if (names.empty()) {
      ADD_FAILURE() << "no fixtures under " << d.data_dir
                    << "; a registered codec with nothing to cut short is a "
                       "codec this sweep does not cover";
      continue;
    }

    for (const std::string & name : names) {
      std::vector<uint8_t> bytes;
      if (!read_file(d.data_dir + "/" + name, bytes) || bytes.empty()) {
        continue;
      }
      fixtures++;
      for (size_t at : cut_points(bytes.size())) {
        Failing f;
        init(f);
        const GIMG_Result r = decode_through(d.name.c_str(), bytes.data(), at, f);
        cuts++;
        if (r == GIMG_OK) { decoded++; } else { refused++; }
        // Any answer is allowed; inventing a new one is not.
        EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_IO ||
            r == GIMG_ERR_FORMAT || r == GIMG_ERR_UNSUPPORTED ||
            r == GIMG_ERR_LIMIT || r == GIMG_ERR_CORRUPT ||
            r == GIMG_ERR_OOM || r == GIMG_ERR_INTERNAL)
            << name << " cut to " << at << " of " << bytes.size()
            << " returned " << (int)r;
        ASSERT_EQ(f.outstanding, 0)
            << name << " cut to " << at << " of " << bytes.size() << " leaked "
            << f.outstanding << " block(s)";
      }
    }
  }

  std::printf("  %ld fixtures, %ld cuts: %ld refused, %ld still decoded\n",
      fixtures, cuts, refused, decoded);
  std::fflush(stdout);
  ASSERT_GT(fixtures, 200) << "only " << fixtures
                           << " fixtures were read - this sweep is not "
                              "looking at what it thinks it is";
  ASSERT_GT(cuts, 10000) << "only " << cuts << " cut points";
  // A sweep where nothing was refused would mean the cuts are landing
  // somewhere harmless, which is the same as not cutting at all.
  ASSERT_GT(refused, cuts / 2)
      << "only " << refused << " of " << cuts
      << " cuts were refused; the cut points are not reaching the data";
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
