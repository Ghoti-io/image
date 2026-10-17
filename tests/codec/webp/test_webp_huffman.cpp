/**
 * @file
 *
 * VP8L Huffman code sets a hostile file can carry: oversubscribed and
 * incomplete codes, all-zero code lengths, a single symbol, codes at the
 * maximum length, a hostile code-length code, code-length runs that leave the
 * alphabet, and meta-Huffman images whose group count and alphabet sizes push
 * the tree storage to its limits. libwebp's CVE-2023-4863 was an input of this
 * class; this decoder walks trees bit by bit instead of building lookup
 * tables, so that overflow has no counterpart here, but the tree build and the
 * code-length reader still decide what to accept.
 *
 * Every refusal is paired with a control: the same stream with the one code
 * made valid, which must decode to known pixels. The hostile stream is built
 * so that, were its code accepted, the rest of it would decode too. That is
 * what makes each refusal the code's doing rather than a truncation or a
 * misplaced bit somewhere else in a hand-built stream.
 *
 * Streams are built bit by bit from the format description (RFC 9649), not
 * from libwebp. Set GIMG_VP8L_DUMP_DIR to have each stream written there as a
 * .webp file, to ask libwebp the same question.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/raster.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "webp_internal.h"

namespace {

/* LSB-first bit writer, the order VP8L reads in. */
struct Bits {
  std::vector<uint8_t> buf;
  size_t n = 0;

  void bit(int b) {
    if (n % 8 == 0) {
      buf.push_back(0);
    }
    if (b) {
      buf.back() |= (uint8_t)(1u << (n % 8));
    }
    ++n;
  }
  /* An n-bit field, least significant bit first. */
  void put(uint32_t v, int nbits) {
    for (int i = 0; i < nbits; ++i) {
      bit((int)((v >> i) & 1u));
    }
  }
  /* A Huffman code, most significant bit of the canonical code first. */
  void code(uint32_t c, int len) {
    for (int i = len - 1; i >= 0; --i) {
      bit((int)((c >> i) & 1u));
    }
  }
};

/* Canonical (DEFLATE-style) codes for a set of lengths; 0 = unused. */
std::vector<uint32_t> canonical(const std::vector<int> & lens) {
  int count[16] = {0}; /* count[0] stays 0: unused symbols take no code */
  uint32_t next[16] = {0};
  std::vector<uint32_t> codes(lens.size(), 0);
  for (int l : lens) {
    if (l > 0) {
      ++count[l];
    }
  }
  uint32_t c = 0;
  for (int len = 1; len < 16; ++len) {
    c = (c + (uint32_t)count[len - 1]) << 1;
    next[len] = c;
  }
  for (size_t s = 0; s < lens.size(); ++s) {
    if (lens[s] > 0) {
      codes[s] = next[lens[s]]++;
    }
  }
  return codes;
}

const int k_order[19] = {
    17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

enum {
  ALPHA_GREEN = 280,
  ALPHA_RGB = 256,
  ALPHA_DIST = 40
};

/* A simple code: one or two symbols. */
void simple(Bits & w, int s1, int s2 = -1) {
  w.bit(1);
  w.bit(s2 >= 0);
  if (s1 < 2) {
    w.bit(0);
    w.put((uint32_t)s1, 1);
  }
  else {
    w.bit(1);
    w.put((uint32_t)s1, 8);
  }
  if (s2 >= 0) {
    w.put((uint32_t)s2, 8);
  }
}

/* A normal code whose code-length code gives each of the lengths 0..15 a
 * four-bit code equal to the length, then every length written out in turn. */
void uniform_lengths(Bits & w, const std::vector<int> & lens) {
  w.bit(0);
  w.put(19 - 4, 4);
  for (int i = 0; i < 19; ++i) {
    w.put(k_order[i] < 16 ? 4u : 0u, 3);
  }
  w.bit(0); /* every symbol, no max_symbol */
  for (int l : lens) {
    w.code((uint32_t)l, 4);
  }
}

std::vector<int> lens_of(size_t alphabet,
    std::initializer_list<std::pair<int, int>> set) {
  std::vector<int> lens(alphabet, 0);
  for (auto & p : set) {
    lens[(size_t)p.first] = p.second;
  }
  return lens;
}

/* VP8L header, no transform, then optionally a colour cache. The caller
 * writes the meta-Huffman bit onward. */
void header(Bits & w, int width, int height, int cache_bits = 0) {
  w.put(0x2f, 8);
  w.put((uint32_t)(width - 1), 14);
  w.put((uint32_t)(height - 1), 14);
  w.bit(1);
  w.put(0, 3);
  w.bit(0); /* no transform */
  if (cache_bits > 0) {
    w.bit(1);
    w.put((uint32_t)cache_bits, 4);
  }
  else {
    w.bit(0);
  }
}

void maybe_dump(const std::string & name, const std::vector<uint8_t> & vp8l) {
  const char * dir = std::getenv("GIMG_VP8L_DUMP_DIR");
  if (!dir) {
    return;
  }
  const uint32_t chunk = (uint32_t)vp8l.size();
  const uint32_t riff = 4 + 8 + chunk + (chunk & 1u);
  std::vector<uint8_t> f;
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      f.push_back((uint8_t)(v >> (8 * i)));
    }
  };
  f.insert(f.end(), {'R', 'I', 'F', 'F'});
  u32(riff);
  f.insert(f.end(), {'W', 'E', 'B', 'P', 'V', 'P', '8', 'L'});
  u32(chunk);
  f.insert(f.end(), vp8l.begin(), vp8l.end());
  if (chunk & 1u) {
    f.push_back(0);
  }
  const std::string path = std::string(dir) + "/" + name + ".webp";
  if (FILE * fp = std::fopen(path.c_str(), "wb")) {
    std::fwrite(f.data(), 1, f.size(), fp);
    std::fclose(fp);
  }
}

/* Decodes, returning the result and, on success, RGBA bytes. */
GIMG_Result decode(const std::string & name, const Bits & w,
    std::vector<uint8_t> * rgba = nullptr) {
  /* Trailing zero bytes so that a stream which runs one code short of its end
   * is not refused for truncation instead of for its code. */
  std::vector<uint8_t> bytes = w.buf;
  bytes.insert(bytes.end(), 8, 0);
  maybe_dump(name, bytes);
  GIMG_Raster * raster = nullptr;
  const GIMG_Result r =
      gimg_webp_vp8l_decode(bytes.data(), bytes.size(), nullptr, &raster);
  if (r == GIMG_OK && rgba) {
    const uint32_t width = gimg_raster_width(raster);
    const uint32_t height = gimg_raster_height(raster);
    const uint8_t * px = (const uint8_t *)gimg_raster_pixels(raster);
    const size_t stride = gimg_raster_stride_bytes(raster);
    rgba->clear();
    for (uint32_t y = 0; y < height; ++y) {
      rgba->insert(rgba->end(), px + y * stride, px + y * stride + 4 * width);
    }
  }
  gimg_raster_destroy(raster);
  return r;
}

/* A 2x1 image whose green code is the one under test; red and blue are the
 * single symbol 0, alpha the single symbol 255, distance unused. Pixel
 * greens g0 and g1 are written with the canonical codes of green_lens. */
Bits two_pixels(const std::vector<int> & green_lens, int g0, int g1) {
  Bits w;
  header(w, 2, 1);
  w.bit(0); /* no meta-Huffman image */
  uniform_lengths(w, green_lens);
  simple(w, 0);
  simple(w, 0);
  simple(w, 255);
  simple(w, 0);
  const std::vector<uint32_t> c = canonical(green_lens);
  for (int g : {g0, g1}) {
    w.code(c[(size_t)g], green_lens[(size_t)g]);
  }
  return w;
}

std::vector<uint8_t> rgba2(int g0, int g1) {
  return {0, (uint8_t)g0, 0, 255, 0, (uint8_t)g1, 0, 255};
}

} // namespace

/* Two codes of length 1 are a complete code; a third oversubscribes it. The
 * third symbol sorts last, so the first two keep their codes and the pixels
 * would decode the same were the third accepted. */
TEST(WebpHuffman, OversubscribedShortCodeRefused) {
  std::vector<uint8_t> px;
  const auto ok = lens_of(ALPHA_GREEN, {{10, 1}, {20, 1}});
  ASSERT_EQ(decode("over1_control", two_pixels(ok, 10, 20), &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(10, 20));

  const auto over = lens_of(ALPHA_GREEN, {{10, 1}, {20, 1}, {30, 1}});
  EXPECT_EQ(decode("over1", two_pixels(over, 10, 20)), GIMG_ERR_CORRUPT);
}

/* Lengths 1..14 once each and two of length 15 fill the code exactly, and
 * the control decodes a fifteen-bit code. A third length-15 code overflows
 * only at the last length, which the per-length count does not check. */
TEST(WebpHuffman, OversubscribedAtMaximumLengthRefused) {
  std::vector<int> lens(ALPHA_GREEN, 0);
  for (int k = 1; k <= 14; ++k) {
    lens[(size_t)k] = k;
  }
  lens[15] = 15;
  lens[16] = 15;
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("over15_control", two_pixels(lens, 1, 16), &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(1, 16));
  ASSERT_EQ(decode("over15_control_b", two_pixels(lens, 14, 15), &px),
      GIMG_OK);
  EXPECT_EQ(px, rgba2(14, 15));

  lens[17] = 15;
  EXPECT_EQ(decode("over15", two_pixels(lens, 1, 16)), GIMG_ERR_CORRUPT);
}

/* Lengths 1, 2, 2 are complete; dropping one leaves a branch with no leaf.
 * The pixels use only the codes that survive. */
TEST(WebpHuffman, IncompleteCodeRefused) {
  std::vector<uint8_t> px;
  const auto ok = lens_of(ALPHA_GREEN, {{10, 1}, {20, 2}, {30, 2}});
  ASSERT_EQ(decode("incomplete_control", two_pixels(ok, 10, 20), &px),
      GIMG_OK);
  EXPECT_EQ(px, rgba2(10, 20));

  const auto holed = lens_of(ALPHA_GREEN, {{10, 1}, {20, 2}});
  EXPECT_EQ(decode("incomplete", two_pixels(holed, 10, 20)),
      GIMG_ERR_CORRUPT);
}

/* One non-zero length is a single symbol coded in zero bits, whatever the
 * length says. */
TEST(WebpHuffman, SingleSymbolNormalCodeTakesNoBits) {
  for (int len : {1, 7, 15}) {
    std::vector<int> lens(ALPHA_GREEN, 0);
    lens[42] = len;
    Bits w;
    header(w, 2, 1);
    w.bit(0);
    uniform_lengths(w, lens);
    simple(w, 0);
    simple(w, 0);
    simple(w, 255);
    simple(w, 0);
    std::vector<uint8_t> px;
    ASSERT_EQ(decode("single_len" + std::to_string(len), w, &px), GIMG_OK)
        << "length " << len;
    EXPECT_EQ(px, rgba2(42, 42));
  }
}

/* A simple code naming the same symbol twice. */
TEST(WebpHuffman, SimpleCodeRepeatedSymbol) {
  Bits w;
  header(w, 2, 1);
  w.bit(0);
  simple(w, 77, 77);
  simple(w, 0);
  simple(w, 0);
  simple(w, 255);
  simple(w, 0);
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("simple_twice", w, &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(77, 77));
}

/* Every code length zero, in a tree the pixels never read: the distance
 * code of an image made only of literals. Refused on its own account. */
TEST(WebpHuffman, AllZeroCodeLengthsRefused) {
  auto build = [](bool zero) {
    Bits w;
    header(w, 2, 1);
    w.bit(0);
    simple(w, 10, 20);
    simple(w, 0);
    simple(w, 0);
    simple(w, 255);
    if (zero) {
      uniform_lengths(w, std::vector<int>(ALPHA_DIST, 0));
    }
    else {
      uniform_lengths(w, lens_of(ALPHA_DIST, {{0, 1}, {1, 1}}));
    }
    w.code(0, 1);
    w.code(1, 1);
    return w;
  };
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("allzero_control", build(false), &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(10, 20));
  EXPECT_EQ(decode("allzero", build(true)), GIMG_ERR_CORRUPT);
}

/* A simple code's symbols are read in eight bits, and the distance alphabet
 * has forty. A 2x2 image whose second row is a copy of the first, one row
 * up, so the distance code is read: green codes literals 10 and 20 and the
 * length symbol 257 (a copy of two), and the distance code is the simple code
 * under test. Distance symbol 0 is distance code 1, one row up.
 *
 * libwebp 1.5.0 drops a symbol past the alphabet, so a pair with one such
 * symbol is the single other symbol, and a lone one leaves the code empty,
 * which it refuses. The format description does not say; this follows
 * libwebp, and the dump of these streams is how that was checked. */
TEST(WebpHuffman, SimpleSymbolPastAlphabet) {
  auto build = [](int s1, int s2) {
    const auto green = lens_of(ALPHA_GREEN, {{10, 2}, {20, 2}, {257, 1}});
    const std::vector<uint32_t> c = canonical(green);
    Bits w;
    header(w, 2, 2);
    w.bit(0);
    uniform_lengths(w, green);
    simple(w, 0);
    simple(w, 0);
    simple(w, 255);
    simple(w, s1, s2);
    w.code(c[10], 2);
    w.code(c[20], 2);
    w.code(c[257], 1); /* length 2, no extra bits; distance takes 0 bits */
    return w;
  };
  const std::vector<uint8_t> want = {
      0, 10, 0, 255, 0, 20, 0, 255, 0, 10, 0, 255, 0, 20, 0, 255};
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("simple_dist0_control", build(0, -1), &px), GIMG_OK);
  EXPECT_EQ(px, want);
  ASSERT_EQ(decode("simple_dist0_200", build(0, 200), &px), GIMG_OK);
  EXPECT_EQ(px, want);
  ASSERT_EQ(decode("simple_dist200_0", build(200, 0), &px), GIMG_OK);
  EXPECT_EQ(px, want);
  ASSERT_EQ(decode("simple_dist0_40", build(0, 40), &px), GIMG_OK);
  EXPECT_EQ(px, want);

  EXPECT_EQ(decode("simple_dist40", build(40, -1)), GIMG_ERR_CORRUPT);
  EXPECT_EQ(decode("simple_dist255", build(255, -1)), GIMG_ERR_CORRUPT);
  EXPECT_EQ(decode("simple_dist40_255", build(40, 255)), GIMG_ERR_CORRUPT);
}

namespace {

/* The green code written through a code-length code with the given lengths
 * for code-length symbols 0..18. Symbols 0..14 always get the four-bit codes
 * 0..14, so the green lengths (which use only 0, 1 and 2) decode the same
 * however the rest of the code-length code is spoiled. */
Bits through_length_code(const int clc[19]) {
  const auto green = lens_of(ALPHA_GREEN, {{10, 1}, {20, 1}});
  Bits w;
  header(w, 2, 1);
  w.bit(0);
  w.bit(0);
  w.put(19 - 4, 4);
  for (int i = 0; i < 19; ++i) {
    w.put((uint32_t)clc[k_order[i]], 3);
  }
  w.bit(0);
  for (int l : green) {
    w.code((uint32_t)l, 4);
  }
  simple(w, 0);
  simple(w, 0);
  simple(w, 255);
  simple(w, 0);
  w.code(0, 1);
  w.code(1, 1);
  return w;
}

} // namespace

TEST(WebpHuffman, HostileCodeLengthCodeRefused) {
  int clc[19] = {0};
  for (int s = 0; s < 16; ++s) {
    clc[s] = 4;
  }
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("clc_control", through_length_code(clc), &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(10, 20));

  /* Seventeen four-bit codes. */
  clc[17] = 4;
  EXPECT_EQ(decode("clc_over", through_length_code(clc)), GIMG_ERR_CORRUPT);

  /* Fifteen four-bit codes. */
  clc[17] = 0;
  clc[15] = 0;
  EXPECT_EQ(decode("clc_incomplete", through_length_code(clc)),
      GIMG_ERR_CORRUPT);

  /* No code at all. */
  for (int s = 0; s < 19; ++s) {
    clc[s] = 0;
  }
  EXPECT_EQ(decode("clc_zero", through_length_code(clc)), GIMG_ERR_CORRUPT);
}

namespace {

/* A distance code written as: symbol 0 length 1, symbol 1 length 1, then a
 * code 18 (a run of zeros) of the given length, with max_symbol set to
 * max_symbol when it is non-negative. The code-length code is lengths 0..14
 * and 18 at four bits each, so 18 has the code 15. */
Bits distance_runs(int zero_run, int max_symbol) {
  Bits w;
  header(w, 2, 1);
  w.bit(0);
  simple(w, 10, 20);
  simple(w, 0);
  simple(w, 0);
  simple(w, 255);

  w.bit(0);
  w.put(19 - 4, 4);
  for (int i = 0; i < 19; ++i) {
    const int s = k_order[i];
    w.put((s < 15 || s == 18) ? 4u : 0u, 3);
  }
  if (max_symbol >= 0) {
    /* length_nbits = 2 + 2 * 3 = 8; max_symbol = 2 + field. */
    w.bit(1);
    w.put(3, 3);
    w.put((uint32_t)(max_symbol - 2), 8);
  }
  else {
    w.bit(0);
  }
  w.code(1, 4);
  w.code(1, 4);
  w.code(15, 4);
  w.put((uint32_t)(zero_run - 11), 7);

  w.code(0, 1);
  w.code(1, 1);
  return w;
}

} // namespace

/* A run that ends exactly at the alphabet's end is valid; one symbol more
 * writes past it. max_symbol counts code-length codes read, so 3 here reads
 * the two lengths and the run and stops; one more than the alphabet is
 * refused before anything is read. */
TEST(WebpHuffman, CodeLengthRunsStayInsideAlphabet) {
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("run_control", distance_runs(38, -1), &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(10, 20));
  EXPECT_EQ(decode("run_over", distance_runs(39, -1)), GIMG_ERR_CORRUPT);

  ASSERT_EQ(decode("maxsym_control", distance_runs(38, 3), &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(10, 20));
  ASSERT_EQ(decode("maxsym_40", distance_runs(38, 40), &px), GIMG_OK);
  EXPECT_EQ(px, rgba2(10, 20));
  EXPECT_EQ(decode("maxsym_41", distance_runs(38, 41)), GIMG_ERR_CORRUPT);
}

/* The largest alphabet: an eleven-bit colour cache makes the green alphabet
 * 280 + 2048 symbols. Its last symbol, cache index 2047, is coded here, after
 * a literal that hashes to that index so the cached pixel is the literal and
 * not an empty slot's zero. A twelve-bit cache is outside the format. */
TEST(WebpHuffman, LargestGreenAlphabet) {
  const uint32_t g = 3;
  const uint32_t r = 107;
  const uint32_t argb = 0xff000000u | (r << 16) | (g << 8);
  ASSERT_EQ((argb * 0x1e35a7bdu) >> (32 - 11), 2047u);

  const size_t alphabet = ALPHA_GREEN + 2048;
  std::vector<int> lens(alphabet, 0);
  lens[g] = 1;
  lens[alphabet - 1] = 1;
  Bits w;
  header(w, 2, 1, 11);
  w.bit(0);
  uniform_lengths(w, lens);
  simple(w, (int)r);
  simple(w, 0);
  simple(w, 255);
  simple(w, 0);
  w.code(0, 1); /* the literal */
  w.code(1, 1); /* cache index 2047 */
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("cache11", w, &px), GIMG_OK);
  const uint8_t one[4] = {(uint8_t)r, (uint8_t)g, 0, 255};
  EXPECT_EQ(px, (std::vector<uint8_t>{one[0], one[1], one[2], one[3], one[0],
                    one[1], one[2], one[3]}));

  Bits w12;
  header(w12, 2, 1, 12);
  w12.bit(0);
  EXPECT_EQ(decode("cache12", w12), GIMG_ERR_CORRUPT);
}

namespace {

/* A meta-Huffman image: a precision-2 entropy image one tile high whose
 * tile t selects group groups_of[t]; then num_groups groups, each with a
 * green code of all 256 literals at length 8 when full_green(g), else the
 * single literal 0, and a red code that is the single symbol g & 0xff. The
 * main image's pixels are green 2t+1 in tile t (only full groups are
 * referenced, so every pixel's green costs eight bits). */
Bits meta_image(const std::vector<int> & groups_of, int num_groups,
    const std::function<bool(int)> & full_green, int bad_group = -1) {
  const int tiles = (int)groups_of.size();
  Bits w;
  header(w, 4 * tiles, 4);
  w.bit(1);
  w.put(0, 3); /* precision 2: four-pixel tiles */

  /* The entropy image: no cache, one group, green and red at length 8. */
  std::vector<int> g8(ALPHA_GREEN, 0);
  for (int s = 0; s < 256; ++s) {
    g8[(size_t)s] = 8;
  }
  const std::vector<int> r8(ALPHA_RGB, 8);
  w.bit(0);
  uniform_lengths(w, g8);
  uniform_lengths(w, r8);
  simple(w, 0);
  simple(w, 0);
  simple(w, 0);
  for (int t = 0; t < tiles; ++t) {
    w.code((uint32_t)(groups_of[(size_t)t] & 0xff), 8);
    w.code((uint32_t)(groups_of[(size_t)t] >> 8), 8);
  }

  for (int g = 0; g < num_groups; ++g) {
    if (g == bad_group) {
      uniform_lengths(w, lens_of(ALPHA_GREEN, {{1, 1}, {2, 1}, {3, 1}}));
    }
    else if (full_green(g)) {
      uniform_lengths(w, g8);
    }
    else {
      simple(w, 0);
    }
    simple(w, g & 0xff);
    simple(w, 0);
    simple(w, 255);
    simple(w, 0);
  }

  for (int y = 0; y < 4; ++y) {
    for (int t = 0; t < tiles; ++t) {
      for (int x = 0; x < 4; ++x) {
        w.code((uint32_t)((2 * t + 1) & 0xff), 8);
      }
    }
  }
  return w;
}

void expect_meta_pixels(const std::vector<uint8_t> & px,
    const std::vector<int> & groups_of) {
  const size_t tiles = groups_of.size();
  ASSERT_EQ(px.size(), 4 * 4 * tiles * 4);
  for (size_t y = 0; y < 4; ++y) {
    for (size_t t = 0; t < tiles; ++t) {
      for (size_t x = 0; x < 4; ++x) {
        const uint8_t * p = &px[4 * (y * 4 * tiles + 4 * t + x)];
        ASSERT_EQ(p[0], (uint8_t)(groups_of[t] & 0xff)) << "tile " << t;
        ASSERT_EQ(p[1], (uint8_t)(2 * t + 1)) << "tile " << t;
        ASSERT_EQ(p[2], 0);
        ASSERT_EQ(p[3], 255);
      }
    }
  }
}

} // namespace

/* Three hundred groups, each with a 256-leaf green tree: about 76,500 tree
 * nodes. The node pool was a fixed 65,536 nodes, so this valid stream - and
 * real libwebp output of about this many groups - was refused as corrupt. */
TEST(WebpHuffman, ManyGroupsWithLargeTrees) {
  std::vector<int> groups_of;
  for (int t = 0; t < 300; ++t) {
    groups_of.push_back(t);
  }
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("groups300",
                meta_image(groups_of, 300, [](int) { return true; }), &px),
      GIMG_OK);
  expect_meta_pixels(px, groups_of);
}

/* More than a thousand groups declared and two referenced: the rest are read
 * and validated but not kept. Each discarded group here has a 256-leaf green
 * tree, so they must not hold on to tree storage. */
TEST(WebpHuffman, UnreferencedGroupsReadAndDiscarded) {
  const std::vector<int> groups_of = {0, 1100};
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("groups1101_sparse",
                meta_image(groups_of, 1101, [](int) { return true; }), &px),
      GIMG_OK);
  expect_meta_pixels(px, groups_of);

  /* A discarded group's code is still validated. */
  EXPECT_EQ(decode("groups1101_bad_discard",
                meta_image(groups_of, 1101, [](int) { return true; }, 500)),
      GIMG_ERR_CORRUPT);
}

/* The largest group index the entropy image can name is 65535, so 65536
 * groups follow, every one of them read. */
TEST(WebpHuffman, LargestGroupIndex) {
  const std::vector<int> groups_of = {65535, 0};
  std::vector<uint8_t> px;
  ASSERT_EQ(decode("group65535",
                meta_image(groups_of, 65536,
                    [](int g) { return g == 0 || g == 65535; }),
                &px),
      GIMG_OK);
  expect_meta_pixels(px, groups_of);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
