/**
 * @file
 *
 * What GIMG_Limits actually does, field by field and codec by codec.
 *
 * Six caps are declared in stream.h and the manual describes all six. Before
 * this file two of them had a test each - both BMP, both `max_memory` - and
 * the rest were exercised by nothing. A cap that is never asked to refuse and
 * a cap that no code reads look identical from outside, which is the reason
 * this sweep asserts the *bound* rather than the refusal: a file is offered
 * the cap it exactly needs and then one less, and only a pair that answers OK
 * and then LIMIT says a cap is there and is placed where it says.
 *
 * The matrix below is deliberately explicit. Four of the six caps are read by
 * some codecs and not others, and two are read by nothing at all; writing
 * that down as a table is what makes a gap visible here instead of looking
 * like a fixture that happened not to trip anything.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <dirent.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

bool read_file(const std::string & dir, const std::string & name,
    std::vector<uint8_t> & out) {
  const std::string path = dir + "/" + name;
  FILE * fp = fopen(path.c_str(), "rb");
  if (!fp) { return false; }
  fseek(fp, 0, SEEK_END);
  const long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t)n : 0u);
  const bool ok =
      n >= 0 && fread(out.data(), 1u, (size_t)n, fp) == (size_t)n;
  fclose(fp);
  return ok;
}

struct Fixture {
  const char * codec;
  std::string dir;
  std::string name;
  std::vector<uint8_t> bytes;
  size_t items;   ///< Items the document holds when nothing is capped.
  size_t pixels;  ///< Widest item's width * height.
};

/** What a run did, whichever stage stopped it. */
struct Outcome {
  GIMG_Result result;
  size_t items;
  size_t pixels;
};

/**
 * Load @p bytes and decode every item, with @p limits on both.
 *
 * Both, because which stage enforces a cap is a codec's business: BMP reads
 * max_decoded_pixels while parsing the header and PNG reads it while decoding
 * a frame, and a test that watched only one of them would report the other as
 * unenforced.
 */
Outcome run_with(const std::vector<uint8_t> & bytes, const GIMG_Limits * limits) {
  Outcome out = {GIMG_ERR_INTERNAL, 0u, 0u};
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return out;
  }
  GIMG_Load_Options lo = {};
  lo.limits = limits;
  GIMG_Doc * doc = nullptr;
  out.result = gimg_doc_load(in, &lo, nullptr, &doc);
  if (out.result == GIMG_OK && doc) {
    out.items = gimg_doc_item_count(doc);
    GIMG_Decode_Options dopt = {};
    dopt.limits = limits;
    for (size_t i = 0; i < out.items && out.result == GIMG_OK; i++) {
      GIMG_Item * item = gimg_doc_item(doc, i);
      GIMG_Raster * raster = nullptr;
      out.result = item ? gimg_item_decode(item, &dopt, &raster)
                        : GIMG_ERR_INTERNAL;
      if (raster) {
        const size_t px = (size_t)gimg_raster_width(raster) *
            (size_t)gimg_raster_height(raster);
        if (px > out.pixels) { out.pixels = px; }
        gimg_raster_destroy(raster);
      }
    }
  }
  if (doc) { gimg_doc_destroy(doc); }
  gimg_stream_destroy(in);
  return out;
}

/** Every fixture that loads and decodes with nothing capped. */
const std::vector<Fixture> & corpus(void) {
  static std::vector<Fixture> all;
  static bool built = false;
  if (built) { return all; }
  built = true;
  const std::string root = std::string(GIMG_TEST_DATA_JPEG) + "/..";
  struct Dir {
    const char * codec;
    std::string path;
    const char * ext;
  } dirs[] = {
      {"jpeg", std::string(GIMG_TEST_DATA_JPEG), ".jpg"},
      {"png", std::string(GIMG_TEST_DATA_PNG), ".png"},
      {"bmp", root + "/bmp", ".bmp"},
      {"gif", root + "/gif", ".gif"},
  };
  for (const Dir & d : dirs) {
    DIR * dp = opendir(d.path.c_str());
    if (!dp) { continue; }
    std::vector<std::string> names;
    while (struct dirent * e = readdir(dp)) {
      const std::string n = e->d_name;
      const size_t el = strlen(d.ext);
      if (n.size() > el && n.compare(n.size() - el, el, d.ext) == 0) {
        names.push_back(n);
      }
    }
    closedir(dp);
    std::sort(names.begin(), names.end());
    for (const std::string & n : names) {
      Fixture f;
      f.codec = d.codec;
      f.dir = d.path;
      f.name = n;
      if (!read_file(d.path, n, f.bytes)) { continue; }
      // Plenty of these are deliberately malformed; a file that does not come
      // back on its own cannot say anything about a cap.
      const Outcome clean = run_with(f.bytes, nullptr);
      if (clean.result != GIMG_OK || clean.pixels == 0u) { continue; }
      f.items = clean.items;
      f.pixels = clean.pixels;
      all.push_back(f);
    }
  }
  return all;
}

GIMG_Limits none(void) {
  GIMG_Limits l;
  gimg_limits_default(&l);
  return l;
}

} // namespace

/**
 * The pixel cap sits where it says: the image passes at its own size and is
 * refused one pixel below it.
 *
 * Every codec reads this one, which is what makes it the cap worth asserting
 * exactly. Setting it to something absurdly small would be refused by a cap
 * placed anywhere at all.
 */
TEST(Limits, APixelCapRefusesTheImageItIsOneShortOf) {
  long checked = 0, single = 0;
  long by_codec[4] = {0, 0, 0, 0};
  for (const Fixture & f : corpus()) {
    GIMG_Limits at = none();
    at.max_decoded_pixels = f.pixels;
    const Outcome fits = run_with(f.bytes, &at);
    EXPECT_EQ(fits.result, GIMG_OK)
        << f.name << ": refused a cap of exactly its own " << f.pixels
        << " pixels";

    // A one-pixel image has no cap below its own size to be offered: zero is
    // how GIMG_Limits spells "no limit", so the value one short of one means
    // the opposite of what this half is asking. Twenty-four fixtures here are
    // 1x1, and every one of them looked like a cap that did not fire.
    if (f.pixels < 2u) { single++; continue; }

    GIMG_Limits under = none();
    under.max_decoded_pixels = f.pixels - 1u;
    const Outcome tight = run_with(f.bytes, &under);
    EXPECT_EQ(tight.result, GIMG_ERR_LIMIT)
        << f.name << ": a cap of " << (f.pixels - 1u) << " let a " << f.pixels
        << "-pixel image through with result " << (int)tight.result;
    checked++;
    const std::string c = f.codec;
    by_codec[c == "jpeg" ? 0 : c == "png" ? 1 : c == "bmp" ? 2 : 3]++;
  }
  std::printf("  %ld fixtures capped one pixel short of their own size "
              "(jpeg %ld, png %ld, bmp %ld, gif %ld); %ld are 1x1 and cannot "
              "express such a cap\n",
      checked, by_codec[0], by_codec[1], by_codec[2], by_codec[3], single);
  // Every codec, not just a total: a cap read by three of the four would
  // still make a healthy-looking number here.
  for (int i = 0; i < 4; i++) {
    EXPECT_GT(by_codec[i], 0)
        << "no fixture of codec index " << i
        << " reached the pixel cap, so this sweep says nothing about it";
  }
}

/**
 * The frame cap does the same for a document's item count.
 *
 * Only PNG and GIF read it - see the note on the BMP row below - so the sweep
 * says which codecs it found multi-item files for rather than requiring all
 * four.
 */
TEST(Limits, AFrameCapRefusesTheFileItIsOneShortOf) {
  long checked = 0, png = 0, gif = 0;
  for (const Fixture & f : corpus()) {
    if (f.items < 2u) { continue; }
    const std::string c = f.codec;
    // BMP's bitmap arrays make a multi-item document too, and bmp_load.c
    // reads no frame cap: the array is walked to its end whatever the caller
    // asked for. That is a gap in the limits, not in this sweep, and it is
    // recorded in notes/image/open-questions.md.
    if (c != "png" && c != "gif") { continue; }
    GIMG_Limits at = none();
    at.max_frame_count = f.items;
    EXPECT_EQ(run_with(f.bytes, &at).result, GIMG_OK)
        << f.name << ": refused a cap of exactly its own " << f.items
        << " frames";
    GIMG_Limits under = none();
    under.max_frame_count = f.items - 1u;
    EXPECT_EQ(run_with(f.bytes, &under).result, GIMG_ERR_LIMIT)
        << f.name << ": a cap of " << (f.items - 1u) << " let " << f.items
        << " frames through";
    checked++;
    (c == "png" ? png : gif)++;
  }
  std::printf("  %ld multi-frame fixtures capped at their own frame count "
              "(png %ld, gif %ld)\n",
      checked, png, gif);
  EXPECT_GT(png, 0) << "no multi-frame PNG reached the frame cap";
  EXPECT_GT(gif, 0) << "no multi-frame GIF reached the frame cap";
}

/**
 * A chunk cap of one byte refuses every file whose format has chunks.
 *
 * Unlike the two above this one has no exact bound to assert from outside:
 * the quantity it compares against is a segment length the caller never sees.
 * What it can say is that the cap is read at all, and by which codecs.
 */
TEST(Limits, AChunkCapOfOneByteRefusesEveryChunkedFormat) {
  long jpeg = 0, png = 0, gif = 0, bmp_through = 0, bmp_embedded = 0;
  for (const Fixture & f : corpus()) {
    GIMG_Limits tiny = none();
    tiny.max_chunk_size = 1u;
    const GIMG_Result r = run_with(f.bytes, &tiny).result;
    const std::string c = f.codec;
    if (c == "bmp") {
      // bmp_load.c reads no chunk cap: a DIB has no chunk structure to cap.
      // Five fixtures here are the exception and are meant to be - a BMP that
      // wraps a PNG or a JPEG hands the bytes to that codec, which reads the
      // cap the caller set. Counted rather than skipped, so that a change on
      // either side of that shows up as a moved number.
      (r == GIMG_OK ? bmp_through : bmp_embedded)++;
      continue;
    }
    EXPECT_EQ(r, GIMG_ERR_LIMIT)
        << f.name << ": a one-byte chunk cap let the file through with "
        << (int)r;
    (c == "jpeg" ? jpeg : c == "png" ? png : gif)++;
  }
  std::printf("  one-byte chunk cap refused jpeg %ld, png %ld, gif %ld; "
              "%ld bmp fixtures read it as no cap at all and %ld passed it "
              "down to an embedded codec\n",
      jpeg, png, gif, bmp_through, bmp_embedded);
  EXPECT_GT(jpeg, 0);
  EXPECT_GT(png, 0);
  EXPECT_GT(gif, 0);
  EXPECT_GT(bmp_through, 0)
      << "no BMP fixture went through a one-byte chunk cap, which is what "
         "this test records BMP as doing";
  EXPECT_GT(bmp_embedded, 0)
      << "no BMP fixture passed the chunk cap down to an embedded codec, so "
         "the wrapped-PNG and wrapped-JPEG fixtures are not in this corpus";
}

/**
 * The memory cap, which only BMP reads.
 *
 * A byte is below anything, so what this asserts is presence: BMP refuses and
 * the other three do not notice. The asymmetry is the finding - a caller who
 * sets max_memory to bound a decode gets a bound on BMP and nothing on the
 * other three formats, which is not what the manual's one-line description
 * leads them to expect.
 */
TEST(Limits, AMemoryCapIsReadByBmpAndByNoOtherCodec) {
  long bmp_refused = 0, others_through = 0, others_refused = 0;
  for (const Fixture & f : corpus()) {
    GIMG_Limits tiny = none();
    tiny.max_memory = 1u;
    const GIMG_Result r = run_with(f.bytes, &tiny).result;
    if (std::string(f.codec) == "bmp") {
      EXPECT_EQ(r, GIMG_ERR_LIMIT)
          << f.name << ": a one-byte memory cap let the file through";
      bmp_refused++;
    }
    else if (r == GIMG_OK) { others_through++; }
    else { others_refused++; }
  }
  std::printf("  one-byte memory cap: bmp refused %ld; "
              "%ld non-bmp fixtures went through, %ld were refused\n",
      bmp_refused, others_through, others_refused);
  EXPECT_GT(bmp_refused, 0);
  EXPECT_EQ(others_refused, 0)
      << "a codec other than BMP refused on max_memory; the matrix in this "
         "file and the note in stream.h both need updating";
}

/**
 * The two caps no code reads.
 *
 * max_metadata_size and max_recursion are declared in GIMG_Limits and
 * described in the manual, and `grep -rn 'limits->' src/` finds no reader for
 * either. This is the record of that, written as an assertion so that the day
 * one of them is implemented this test fails and has to be replaced by the
 * bound assertions above rather than quietly continuing to pass.
 */
TEST(Limits, TwoOfTheSixCapsAreReadByNothing) {
  long swept = 0;
  for (const Fixture & f : corpus()) {
    GIMG_Limits tiny = none();
    tiny.max_metadata_size = 1u;
    tiny.max_recursion = 1u;
    EXPECT_EQ(run_with(f.bytes, &tiny).result, GIMG_OK)
        << f.name << ": something now reads max_metadata_size or "
                     "max_recursion - replace this test with a bound "
                     "assertion and drop the warning from stream.h";
    swept++;
  }
  std::printf("  %ld fixtures unaffected by max_metadata_size "
              "and max_recursion\n", swept);
  ASSERT_GT(swept, 100)
      << "only " << swept << " fixtures reached this, which is too few for "
                             "the claim it makes";
}

/**
 * A cap set at load time still applies to a decode that carries none.
 *
 * A load only parses headers; the pixels come later, so a limit that did not
 * survive to the decode would bound nothing that actually allocates. The
 * library leans on this itself - gimg_*_save re-decodes its source item with
 * no options of its own - and the arm that copies a caller's other decode
 * options before substituting the remembered limits had never run, because
 * every test either passed limits to both calls or to neither.
 */
TEST(Limits, ACapSetAtLoadTimeSurvivesADecodeThatSaysNothing) {
  long with_null = 0, with_empty = 0;
  for (const Fixture & f : corpus()) {
    if (f.pixels < 2u) { continue; }
    GIMG_Limits under = none();
    under.max_decoded_pixels = f.pixels - 1u;

    for (int shape = 0; shape < 2; shape++) {
      GIMG_Stream * in = nullptr;
      ASSERT_EQ(
          gimg_stream_create_memory(f.bytes.data(), f.bytes.size(), &in),
          GIMG_OK);
      GIMG_Load_Options lo = {};
      lo.limits = &under;
      GIMG_Doc * doc = nullptr;
      GIMG_Result r = gimg_doc_load(in, &lo, nullptr, &doc);
      // Some codecs refuse at the header, which is the cap working one stage
      // earlier and not what this test is about.
      if (r == GIMG_OK && doc) {
        const size_t n = gimg_doc_item_count(doc);
        // shape 0: no options at all. shape 1: options that mention no
        // limits, which is the arm that has to copy them before adding the
        // remembered ones.
        GIMG_Decode_Options empty = {};
        for (size_t i = 0; i < n && r == GIMG_OK; i++) {
          GIMG_Item * item = gimg_doc_item(doc, i);
          GIMG_Raster * raster = nullptr;
          r = gimg_item_decode(item, shape == 0 ? nullptr : &empty, &raster);
          if (raster) { gimg_raster_destroy(raster); }
        }
        EXPECT_EQ(r, GIMG_ERR_LIMIT)
            << f.name << ": a cap of " << (f.pixels - 1u)
            << " set on the load did not reach a decode that "
            << (shape == 0 ? "passed no options" : "passed empty options");
        (shape == 0 ? with_null : with_empty)++;
      }
      if (doc) { gimg_doc_destroy(doc); }
      gimg_stream_destroy(in);
    }
  }
  std::printf("  the load's cap reached %ld decodes given no options and "
              "%ld given empty ones\n", with_null, with_empty);
  EXPECT_GT(with_null, 0);
  EXPECT_GT(with_empty, 0);
}

/**
 * The codec registry can be walked by index as well as by name.
 *
 * gimg_codec_by_index() is public and was called by nothing, so neither it
 * nor its out-of-range arm had run. Walking it is also the only way to assert
 * that the registry holds what this library says it ships.
 */
// This list is deliberately written out, and it is the only place in the tests
// that names the codecs. It is the inventory assertion: its job is to state
// what this library ships, which a version read back from the registry could
// not do. Every sweep that should *cover* every codec takes its population
// from the registry instead - see tests/registry_sweep.h. Adding a format
// means editing this one list, on purpose.
const std::vector<std::string> kShippedCodecs = {"bmp", "gif", "jpeg", "png"};

TEST(Limits, TheRegistryHoldsEveryShippedCodecAndIsWalkableByIndex) {
  const size_t n = gimg_codec_count();
  ASSERT_EQ(n, kShippedCodecs.size())
      << "the registry holds " << n << " codecs; this library ships "
      << kShippedCodecs.size()
      << ". If a codec was just added, add it to kShippedCodecs above";
  std::vector<std::string> names;
  for (size_t i = 0; i < n; i++) {
    GIMG_Codec * c = gimg_codec_by_index(i);
    ASSERT_NE(c, nullptr) << "index " << i << " of " << n << " is null";
    const char * name = gimg_codec_name(c);
    ASSERT_NE(name, nullptr);
    names.push_back(name);
    // Both lookups have to agree, or one of them is indexing something else.
    EXPECT_EQ(gimg_codec_by_name(name), c) << "by_name(" << name
                                           << ") is not by_index(" << i << ")";
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, kShippedCodecs);
  EXPECT_EQ(gimg_codec_by_index(n), nullptr) << "one past the end is not null";
  EXPECT_EQ(gimg_codec_by_index((size_t)-1), nullptr);
  EXPECT_EQ(gimg_codec_by_name("no such codec"), nullptr);
  EXPECT_EQ(gimg_codec_by_name(nullptr), nullptr);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
