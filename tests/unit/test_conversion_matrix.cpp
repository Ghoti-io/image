/**
 * @file
 *
 * Every fixture, saved as every format.
 *
 * Converting between formats is the commonest thing a caller does with an
 * image library and the least like the tests written for it, which mostly
 * load a file of one format and save it back as the same one. The gap is not
 * hypothetical: this codec once refused every conversion *into* a JPEG,
 * because the save asked whether the JPEG codec had loaded the document
 * before it would decode the raster, and a PNG or BMP fails that question.
 * Nothing caught it, because nothing converted.
 *
 * So: each of the committed fixtures that decodes at all, saved as PNG, BMP,
 * GIF and JPEG, reloaded and compared. What each pair is held to depends on
 * what the target format can carry, and saying so exactly is most of the
 * value:
 *
 *   - **PNG carries everything this library decodes to**, at 8 or 16 bits,
 *     gray or colour, with or without alpha. So a PNG conversion is exact:
 *     same channel count, same depth, same bytes. Measured over the fixture
 *     set, 237 of 237.
 *   - **GIF carries the visible image.** One palette index is transparent and
 *     the colour stored behind it is not the caller's: a fully transparent
 *     pixel comes back with its alpha intact and its RGB whatever the writer
 *     put in that palette slot. So transparency is compared everywhere and
 *     colour only where the pixel can be seen.
 *   - **BMP has no grayscale and no 16-bit form here**, so a gray or deep
 *     raster comes back widened or narrowed. Where the format did survive,
 *     the pixels must be exact.
 *   - **JPEG is lossy**, so only the geometry is held.
 *
 * And the refusals are held too: a format that cannot represent a raster must
 * say GIMG_ERR_UNSUPPORTED - never a crash, never a claim that the data is
 * corrupt. PNG and BMP refuse the CMYK, YCCK and many-component JPEGs, which
 * neither format has a way to store; GIF refuses anything past 256 colours,
 * which is a decision it declines to make rather than a limitation.
 *
 * Checked by mutation - flipping one bit of the PNG writer's output fails the
 * exactness assertion on every PNG conversion. The first plant was inert and
 * that is worth recording: it flipped a byte of the row buffer *before* the
 * loop that fills it, so the fill wrote over it and the test stayed green. A
 * plant that cannot reach the output proves nothing about the assertion, and
 * reads exactly like an assertion that does not work.
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

#include "../registry_sweep.h"
#include <algorithm>
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

struct Decoded {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t channels = 0;
  uint8_t bits = 0;
  std::vector<uint8_t> pixels; ///< Row-major, stride padding removed.
};

bool decode_bytes(const std::vector<uint8_t> & bytes, Decoded & out) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
    return false;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) != GIMG_OK) {
    gimg_stream_destroy(s);
    return false;
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) != GIMG_OK ||
      !raster) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
    return false;
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  out.width = gimg_raster_width(raster);
  out.height = gimg_raster_height(raster);
  out.channels = fmt->channel_count;
  out.bits = fmt->bits_per_channel[0];
  const size_t stride = gimg_raster_stride_bytes(raster);
  const size_t row = (size_t)out.width * gimg_raster_bytes_per_pixel(fmt);
  const auto * p = (const unsigned char *)gimg_raster_pixels_const(raster);
  out.pixels.resize(row * out.height);
  for (uint32_t y = 0; y < out.height; y++) {
    memcpy(out.pixels.data() + (size_t)y * row, p + (size_t)y * stride, row);
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  return true;
}

/** Save the document in @p bytes as @p format; GIMG_OK fills @p out. */
GIMG_Result convert(const std::vector<uint8_t> & bytes, const char * format,
    std::vector<uint8_t> & out) {
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(in);
    return r;
  }
  // The decode is the caller's own first step in a conversion, and the save
  // has to work from whatever it produced.
  gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr);
  GIMG_Stream * sink = nullptr;
  if (gimg_stream_create_memory_output(&sink) != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Save_Options opts = {};
  opts.jpeg_quality = 95;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, sink, format, &opts, &report);
  if (r == GIMG_OK) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(sink, &p, &n);
    out.assign((const uint8_t *)p, (const uint8_t *)p + n);
  }
  gimg_stream_destroy(sink);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  return r;
}

struct Counts {
  long converted = 0;
  long refused = 0;
};

} // namespace

/**
 * A refusal frees the raster the save path decoded for itself.
 *
 * gimg_doc_save() takes the item's attached raster when there is one and
 * decodes its own when there is not, and only in the second case does it own
 * what it is holding. Every refusal after that point has to free it. CMYK is
 * the case that gets there: PNG has no colour type for four inks, and unlike
 * a twelve-bit JPEG - which decodes to a sixteen-bit raster the writer can
 * spell - a CMYK JPEG decodes to a CMYK raster it cannot.
 *
 * The sweep below never reaches this. It calls gimg_item_ensure_decoded()
 * first, which attaches the raster, so the save path is always the borrowing
 * one. The two cases here are that difference and nothing else, and they must
 * agree: whether the caller decoded first cannot change the answer, only who
 * owns the buffer.
 *
 * Neither assertion below is what catches a leak, and it is worth being clear
 * about that. Dropping the free in png_save.c leaves both of them passing and
 * makes the ASan build report 2600 bytes in 2 allocations at exit, failing
 * `make test-asan` on the process status rather than on a check here. What
 * this test contributes is the only input that walks the owning path at all;
 * LeakSanitizer does the rest.
 */
TEST(ConversionMatrix, RefusingACmykFrameFreesTheRasterItDecoded) {
  const std::string path =
      std::string(GIMG_TEST_DATA_JPEG) + "/cmyk_ljt_sub.jpg";
  std::ifstream f(path, std::ios::binary);
  ASSERT_TRUE(f.good()) << "missing fixture: " << path;
  const std::vector<uint8_t> bytes(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  ASSERT_FALSE(bytes.empty());

  auto save_png = [&](bool decode_first) {
    GIMG_Stream * in = nullptr;
    EXPECT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &in),
        GIMG_OK);
    GIMG_Doc * doc = nullptr;
    GIMG_Result r = gimg_doc_load(in, nullptr, nullptr, &doc);
    EXPECT_EQ(r, GIMG_OK);
    if (r != GIMG_OK) {
      gimg_stream_destroy(in);
      return r;
    }
    if (decode_first) {
      gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr);
    }
    GIMG_Stream * sink = nullptr;
    EXPECT_EQ(gimg_stream_create_memory_output(&sink), GIMG_OK);
    GIMG_Save_Options opts = {};
    GIMG_Save_Report report = {};
    r = gimg_doc_save(doc, sink, "png", &opts, &report);
    gimg_stream_destroy(sink);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
    return r;
  };

  EXPECT_EQ(save_png(true), GIMG_ERR_UNSUPPORTED)
      << "control: with the raster already attached, PNG still has no CMYK";
  EXPECT_EQ(save_png(false), GIMG_ERR_UNSUPPORTED)
      << "the save path decoded this one itself, and must free it on the way "
         "out";
}

TEST(ConversionMatrix, EveryFixtureSurvivesEveryFormatThatCanHoldIt) {
  // Sources and targets both come from the registry: a codec added without
  // being converted to and from would leave this matrix passing over the four
  // it was written against.
  std::vector<std::string> dirs;
  std::vector<std::string> targets;
  for (const gimg_test::SweptCodec & c : gimg_test::swept_codecs()) {
    dirs.push_back(c.data_dir);
    if (c.writes()) { targets.push_back(c.name); }
  }
  ASSERT_FALSE(dirs.empty()) << "no codecs registered; nothing to sweep";
  ASSERT_FALSE(targets.empty()) << "no codec can write; nothing to convert to";

  long fixtures = 0;
  std::map<std::string, Counts> counts;
  for (const std::string & dir : dirs) {
    DIR * d = opendir(dir.c_str());
    if (!d) { continue; }
    std::vector<std::string> names;
    while (struct dirent * e = readdir(d)) {
      const std::string n = e->d_name;
      if (n == "." || n == ".." || entry_is_directory(dir, e)) { continue; }
      names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());

    for (const std::string & name : names) {
      std::ifstream f(dir + "/" + name, std::ios::binary | std::ios::ate);
      if (!f) { continue; }
      const std::streamsize size = f.tellg();
      if (size <= 0 || size > (std::streamsize)(16 << 20)) { continue; }
      f.seekg(0);
      std::vector<uint8_t> bytes((size_t)size);
      f.read((char *)bytes.data(), size);

      Decoded before;
      if (!decode_bytes(bytes, before)) {
        continue; // Not a decodable image; the refusal sweep covers those.
      }
      fixtures++;

      for (const std::string & target : targets) {
        SCOPED_TRACE(name + " -> " + target);
        std::vector<uint8_t> converted;
        const GIMG_Result r = convert(bytes, target.c_str(), converted);
        if (r != GIMG_OK) {
          counts[target].refused++;
          EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED)
              << "a format that cannot hold this raster must say so, not "
                 "report it as broken data";
          continue;
        }
        counts[target].converted++;
        Decoded after;
        ASSERT_TRUE(decode_bytes(converted, after))
            << "the file it wrote does not load back";
        ASSERT_EQ(after.width, before.width);
        ASSERT_EQ(after.height, before.height);

        if (target == "jpeg") {
          continue; // Lossy: the geometry is all that is promised.
        }
        if (target == "png") {
          ASSERT_EQ(after.channels, before.channels)
              << "PNG carries every shape this library decodes to";
          ASSERT_EQ(after.bits, before.bits);
          EXPECT_TRUE(after.pixels == before.pixels)
              << "a PNG conversion is exact";
          continue;
        }
        if (after.channels != before.channels || after.bits != before.bits) {
          // BMP has no grayscale and no 16-bit form here, so a gray or deep
          // raster is widened or narrowed on the way in. Geometry only.
          continue;
        }
        if ((target == "gif" || target == "webp") && before.channels == 4 &&
            before.bits == 8) {
          // GIF carries the visible image: one palette index is transparent
          // and the colour stored behind it is the writer's, not the
          // caller's. WebP's default save (webp_exact=0) likewise clears RGB
          // under fully transparent pixels, matching cwebp -noexact.
          // Transparency everywhere, colour where it can be seen.
          const size_t n = before.pixels.size();
          ASSERT_EQ(after.pixels.size(), n);
          for (size_t i = 0; i + 3 < n; i += 4) {
            ASSERT_EQ(after.pixels[i + 3], before.pixels[i + 3])
                << "transparency changed at byte " << i;
            if (before.pixels[i + 3] == 0) { continue; }
            for (size_t k = 0; k < 3; k++) {
              ASSERT_EQ(after.pixels[i + k], before.pixels[i + k])
                  << "a visible pixel changed at byte " << (i + k);
            }
          }
          continue;
        }
        EXPECT_TRUE(after.pixels == before.pixels)
            << "the format survived the trip, so the pixels must too";
      }
    }
  }

  for (const auto & kv : counts) {
    std::printf("  -> %-5s %4ld converted, %4ld refused\n", kv.first.c_str(),
        kv.second.converted, kv.second.refused);
  }
  // The alarm on the sweep: a wrong path finds nothing and passes.
  ASSERT_GT(fixtures, 150)
      << "only " << fixtures << " fixtures decoded, so this is looking in the "
         "wrong place - check the data directories beside "
      << GIMG_TEST_DATA_JPEG;
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
