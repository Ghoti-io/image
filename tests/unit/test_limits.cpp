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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

#include "../registry_sweep.h"

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

/**
 * Every codec the corpus draws fixtures from.
 *
 * One list, read by corpus() to find the files and by the per-codec
 * assertions to know what "every codec" means. They were two lists until
 * 2026-10-02 and the second one was a hard-coded four, which is how a sweep
 * over five codecs came to assert over four of them and label the fifth as
 * the fourth.
 */
const std::vector<const char *> & corpus_codecs(void) {
  static const std::vector<const char *> names = {
      "jpeg", "png", "bmp", "gif", "tiff", "ico", "webp"};
  return names;
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
      // TIFF was missing from this list until 2026-09-28. The codec landed
      // after the sweep was written and nothing widened the population, so
      // every claim this file made about "every codec" was made over four.
      {"tiff", root + "/tiff", ".tif"},
      // ICO and WebP were missing until 2026-10-02, and the comment directly
      // above was already here when they landed. A warning is not a gate: it
      // told the next reader what had gone wrong last time and did not stop
      // it happening again. If a seventh codec arrives and this list still
      // has six entries, that is the same defect a third time - so prefer a
      // check keyed on the registry over another comment.
      {"ico", root + "/ico", ".ico"},
      {"webp", root + "/webp", ".webp"},
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
  // Keyed on the codec's own name rather than an index, because the array this
  // replaced had four slots and a final `: 3` that swept every codec after the
  // third into the bucket labelled "gif". With TIFF in the corpus that label
  // was already wrong, and the all-four-nonzero assertion below it could be
  // satisfied by TIFF alone while no GIF fixture reached the cap at all. A map
  // cannot mislabel a codec it has never heard of.
  std::map<std::string, long> by_codec;
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
    by_codec[f.codec]++;
  }
  std::printf("  %ld fixtures capped one pixel short of their own size (",
      checked);
  for (const auto & kv : by_codec) {
    std::printf("%s %ld ", kv.first.c_str(), kv.second);
  }
  std::printf("); %ld are 1x1 and cannot express such a cap\n", single);
  // Every codec in the corpus, not just a total: a cap read by six of the
  // seven would still make a healthy-looking number here. Driven off the
  // directory list rather than a hard-coded count, so adding a codec to the
  // corpus adds it to this assertion with no second edit.
  for (const char * codec : corpus_codecs()) {
    EXPECT_GT(by_codec[codec], 0)
        << "no " << codec
        << " fixture reached the pixel cap, so this sweep says nothing about it";
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
 * Every registered codec is in this file's corpus.
 *
 * The gate the two stale-population comments in corpus() asked for. Those
 * comments recorded the same defect twice - TIFF landing outside the sweep in
 * September, ICO and WebP in October - and a comment cannot fail a build. This
 * can: the population comes from gimg_codec_count(), so an eighth codec is a
 * failure here on the commit that registers it, naming itself.
 *
 * Armed by deleting one entry from corpus_codecs(): this test then names it.
 */
TEST(Limits, EveryRegisteredCodecIsInTheLimitsCorpus) {
  const std::vector<const char *> & listed = corpus_codecs();
  for (const gimg_test::SweptCodec & c : gimg_test::swept_codecs()) {
    if (!c.reads()) { continue; }
    bool found = false;
    for (const char * name : listed) {
      if (c.name == name) { found = true; break; }
    }
    EXPECT_TRUE(found)
        << "codec '" << c.name
        << "' is registered and reads files, but is not in corpus_codecs(), "
           "so every per-codec claim in this file is made without it";
  }
  // And the other direction: a name listed with no fixtures behind it would
  // make the per-codec assertions vacuous rather than false.
  std::map<std::string, long> seen;
  for (const Fixture & f : corpus()) { seen[f.codec]++; }
  for (const char * name : listed) {
    EXPECT_GT(seen[name], 0)
        << "corpus_codecs() lists '" << name
        << "' but no fixture of it loads, so claims about it say nothing";
  }
  std::printf("  %zu registered codecs, %zu in the corpus\n",
      gimg_test::swept_codecs().size(), listed.size());
}

/**
 * A chunk cap of one byte refuses every file whose format has chunks.
 *
 * Unlike the two above this one has no exact bound to assert from outside:
 * the quantity it compares against is a segment length the caller never sees.
 * What it can say is that the cap is read at all, and by which codecs.
 */
TEST(Limits, AChunkCapOfOneByteRefusesEveryChunkedFormat) {
  long jpeg = 0, png = 0, gif = 0, bmp_through = 0, bmp_embedded = 0,
       tiff_through = 0, tiff_embedded = 0, ico_through = 0, ico_embedded = 0,
       webp_through = 0, webp_capped = 0;
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
    if (c == "tiff") {
      // TIFF is not built out of length-prefixed segments: a tag names an
      // offset into the file and the strips are located the same way, so there
      // is no chunk for this cap to be a cap on. The exception is the same one
      // BMP has - a TIFF whose strips are JPEG hands them to the JPEG codec,
      // which reads the cap the caller set. Counted in two buckets for the
      // same reason BMP's are, so a change on either side is a moved number.
      (r == GIMG_OK ? tiff_through : tiff_embedded)++;
      continue;
    }
    if (c == "ico") {
      // Same shape as BMP, for the same reason and one level up: an icon is a
      // directory, not a chunked format, so there is nothing here for the cap
      // to be a cap on. An entry whose payload is a whole PNG hands the bytes
      // to the PNG codec, which does read it.
      (r == GIMG_OK ? ico_through : ico_embedded)++;
      continue;
    }
    if (c == "webp") {
      // Counted, not asserted, and this is a gap rather than a property.
      // A RIFF file *is* length-prefixed chunks - the one structure this cap
      // was written for - and webp_load.c does not read max_chunk_size. So a
      // one-byte chunk cap lets a WebP through where it refuses a PNG, a JPEG
      // and a GIF. Recorded here as a number so that closing it is a moved
      // number rather than a new test; see documentation/formats/webp.md's
      // "Not implemented".
      (r == GIMG_OK ? webp_through : webp_capped)++;
      continue;
    }
    EXPECT_EQ(r, GIMG_ERR_LIMIT)
        << f.name << ": a one-byte chunk cap let the file through with "
        << (int)r;
    (c == "jpeg" ? jpeg : c == "png" ? png : gif)++;
  }
  std::printf("  one-byte chunk cap refused jpeg %ld, png %ld, gif %ld; "
              "%ld bmp fixtures read it as no cap at all and %ld passed it "
              "down to an embedded codec; %ld tiff have no chunks to cap and "
              "%ld passed it down to an embedded JPEG; %ld ico are a directory "
              "with no chunks and %ld passed it down to an embedded PNG; "
              "%ld webp went through a cap their RIFF chunks do not read and "
              "%ld were capped\n",
      jpeg, png, gif, bmp_through, bmp_embedded, tiff_through, tiff_embedded,
      ico_through, ico_embedded, webp_through, webp_capped);
  EXPECT_GT(jpeg, 0);
  EXPECT_GT(png, 0);
  EXPECT_GT(gif, 0);
  EXPECT_GT(bmp_through, 0)
      << "no BMP fixture went through a one-byte chunk cap, which is what "
         "this test records BMP as doing";
  EXPECT_GT(bmp_embedded, 0)
      << "no BMP fixture passed the chunk cap down to an embedded codec, so "
         "the wrapped-PNG and wrapped-JPEG fixtures are not in this corpus";
  EXPECT_GT(tiff_through, 0)
      << "no TIFF fixture reached this, so the claim that TIFF has no chunk "
         "to cap is being made over an empty set";
  EXPECT_GT(tiff_embedded, 0)
      << "no TIFF fixture passed the chunk cap down to an embedded JPEG, so "
         "the JPEG-in-TIFF fixtures are not in this corpus";
}

/**
 * The memory cap, which the BMP decoder reads and no other decoder does.
 *
 * A byte is below anything, so what this asserts is presence: BMP refuses and
 * the formats with a decoder of their own do not notice. The asymmetry is the
 * finding - a caller who sets max_memory to bound a decode gets a bound on
 * BMP and nothing on the others, which is not what the manual's one-line
 * description leads them to expect.
 *
 * **The third bucket is ICO, and it is not an exception to the rule but a
 * consequence of it.** An icon entry whose payload is a Windows DIB is handed
 * to gimg_bmp_load_dib() with the caller's limits forwarded, so the cap is
 * read - by the BMP decoder, one level down, exactly as the chunk cap is for
 * a BMP that wraps a PNG and for a TIFF whose strips are JPEG. The test was
 * named "...ByNoOtherCodec" and said "the other three" when there were five
 * codecs; widening the corpus to seven on 2026-10-02 made it fail, and the
 * claim rather than the code was what was wrong. Counted in its own bucket so
 * that a *decoder* newly reading the cap is still a failure here.
 */
TEST(Limits, AMemoryCapIsReadByTheBmpDecoderAndNothingElse) {
  long bmp_refused = 0, others_through = 0, others_refused = 0,
       delegated_refused = 0;
  for (const Fixture & f : corpus()) {
    GIMG_Limits tiny = none();
    tiny.max_memory = 1u;
    const GIMG_Result r = run_with(f.bytes, &tiny).result;
    const std::string c = f.codec;
    if (c == "bmp") {
      EXPECT_EQ(r, GIMG_ERR_LIMIT)
          << f.name << ": a one-byte memory cap let the file through";
      bmp_refused++;
    }
    else if (c == "ico") {
      // Either bucket is correct: a PNG-payload entry does not reach the BMP
      // decoder and a DIB-payload one does.
      (r == GIMG_OK ? others_through : delegated_refused)++;
    }
    else if (r == GIMG_OK) { others_through++; }
    else { others_refused++; }
  }
  std::printf("  one-byte memory cap: bmp refused %ld; %ld ico refused it "
              "through the BMP decoder their DIB payload reaches; "
              "%ld other fixtures went through, %ld were refused\n",
      bmp_refused, delegated_refused, others_through, others_refused);
  EXPECT_GT(bmp_refused, 0);
  EXPECT_GT(delegated_refused, 0)
      << "no ICO fixture reached the BMP decoder's memory cap, so the "
         "DIB-payload entries are not in this corpus";
  EXPECT_EQ(others_refused, 0)
      << "a decoder other than BMP's refused on max_memory; the matrix in "
         "this file and the note in stream.h both need updating";
}

/**
 * max_metadata_size bounds what is kept that is not pixels, in every codec.
 *
 * This replaces an assertion that it was read by nothing. That earlier test
 * was right when it was written and is the reason this one exists: it failed
 * the moment a reader appeared, which is what an absence-assertion is for.
 *
 * The cap is set to one byte, which no real profile, comment or description
 * fits in. A fixture that carries none is unaffected and still loads, so the
 * sweep asserts the two outcomes separately rather than accepting either: a
 * cap that refused everything would pass a test that only looked for refusals,
 * and a cap that bounded nothing would pass a test that only looked for
 * successes.
 */
TEST(Limits, MetadataSizeBoundsWhatIsKeptThatIsNotPixels) {
  long swept = 0, refused = 0, unaffected = 0;
  std::map<std::string, long> by_codec;
  for (const Fixture & f : corpus()) {
    GIMG_Limits tiny = none();
    tiny.max_metadata_size = 1u;
    const GIMG_Result r = run_with(f.bytes, &tiny).result;
    EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_LIMIT)
        << f.name << ": max_metadata_size produced " << r
        << ", which is neither keeping the file nor naming the cap";
    if (r == GIMG_ERR_LIMIT) {
      refused++;
      by_codec[f.codec]++;
    }
    else {
      unaffected++;
    }
    swept++;
  }
  std::printf("  %ld fixtures: %ld refused on max_metadata_size, %ld carry "
              "none and are unaffected\n",
      swept, refused, unaffected);
  ASSERT_GT(swept, 100)
      << "only " << swept << " fixtures reached this, which is too few for "
                             "the claim it makes";
  EXPECT_GT(unaffected, 0)
      << "every fixture was refused, so the cap is not distinguishing a file "
         "that carries metadata from one that does not";

  // Per codec, not in total. A total above zero is satisfied by one format,
  // and the claim being made is that every codec reads this cap - which is
  // exactly the claim the old table got wrong about max_frame_count by
  // naming two codecs when three read it.
  //
  // WebP joined this list on 2026-10-02. It was the sixth codec and the only
  // one that did not read the cap: ICCP, EXIF and XMP were bounded by the
  // length of the file and nothing else. ICO is deliberately absent - an icon
  // carries no metadata of its own, and an entry whose payload is a PNG is
  // bounded by the PNG codec reading the same cap one level down.
  for (const char * c : {"png", "jpeg", "bmp", "gif", "tiff", "webp"}) {
    EXPECT_GT(by_codec[c], 0)
        << c
        << " refused nothing on max_metadata_size, so either its "
           "fixtures carry none or it does not read the cap";
    std::printf("    %-5s %ld\n", c, by_codec[c]);
  }
}

/**
 * The struct has no cap that nothing reads.
 *
 * max_recursion was the last one and it is gone. It was declared for a TIFF
 * codec that did not exist yet; that codec exists now, and it walks SubIFDs
 * with an iterative loop one level deep whose total is bounded by
 * max_frame_count, so there is still no recursion depth to cap. A field
 * nothing reads is not a promise, which is the rule the rest of the suite
 * holds to - see libs/security's core.h and libs/archive's GARC_Limits.
 *
 * Written as a sweep over the struct's own size rather than a list of names,
 * so that a field added without a reader has to come past this.
 */
TEST(Limits, EveryDeclaredCapHasAReader) {
  GIMG_Limits l;
  gimg_limits_default(&l);
  // The five that remain, each asserted readable through a codec elsewhere in
  // this file. The sizeof is the guard: a sixth field added here changes it,
  // and whoever adds one has to say which codec reads it.
  EXPECT_EQ(sizeof(GIMG_Limits), sizeof(size_t) * 5u + sizeof(l._reserved))
      << "GIMG_Limits gained or lost a field; name its reader in this file "
         "and in stream.h, or it is a cap that promises nothing";
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
const std::vector<std::string> kShippedCodecs = {
    "bmp", "gif", "ico", "jpeg", "png", "tiff", "webp"};

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
