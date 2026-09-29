/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * VP8L lossless bitstream encode. Effort >= 1 applies subtract-green; the
 * entropy stage is Huffman over literals (no LZ77). Round-trip through our
 * decoder is identity. Output is accepted by libwebp's dwebp.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "webp_internal.h"

enum {
  VP8L_MAGIC = 0x2f,
  VP8L_NUM_LITERAL = 256,
  VP8L_NUM_LENGTH = 24,
  VP8L_NUM_DISTANCE = 40,
  VP8L_NUM_CODE_LENGTH = 19,
  VP8L_MAX_CODE_LENGTH = 15,
  VP8L_GREEN = 0,
  VP8L_RED = 1,
  VP8L_BLUE = 2,
  VP8L_ALPHA = 3,
  VP8L_DIST = 4,
  VP8L_MAX_DIM = 16384,
  VP8L_LIT_ALPHABET = VP8L_NUM_LITERAL + VP8L_NUM_LENGTH,
  VP8L_MAX_ALPHABET = VP8L_LIT_ALPHABET
};

static const uint8_t k_code_length_order[VP8L_NUM_CODE_LENGTH] = {
  17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const uint8_t k_reversed4[16] = {
  0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
  0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf
};

typedef struct {
  uint64_t bits;
  int used;
  uint8_t * buf;
  size_t size;
  size_t cap;
  const GIMG_Allocator * alloc;
  int error;
} vp8l_bw_t;

typedef struct {
  uint8_t * lengths;
  uint16_t * codes;
  int num_symbols;
} huff_code_t;

typedef struct {
  uint8_t code;
  uint8_t extra;
} huff_token_t;

static int bw_grow(vp8l_bw_t * bw, size_t need) {
  size_t ncap;
  uint8_t * nbuf;
  if (bw->error) {
    return 0;
  }
  if (bw->size + need <= bw->cap) {
    return 1;
  }
  ncap = bw->cap ? bw->cap : 256u;
  while (ncap < bw->size + need) {
    ncap *= 2u;
  }
  nbuf = (uint8_t *)gimg_realloc(bw->alloc, bw->buf, ncap);
  if (!nbuf) {
    bw->error = 1;
    return 0;
  }
  bw->buf = nbuf;
  bw->cap = ncap;
  return 1;
}

static void bw_flush32(vp8l_bw_t * bw) {
  if (!bw_grow(bw, 4u)) {
    return;
  }
  bw->buf[bw->size++] = (uint8_t)(bw->bits & 0xffu);
  bw->buf[bw->size++] = (uint8_t)((bw->bits >> 8) & 0xffu);
  bw->buf[bw->size++] = (uint8_t)((bw->bits >> 16) & 0xffu);
  bw->buf[bw->size++] = (uint8_t)((bw->bits >> 24) & 0xffu);
  bw->bits >>= 32;
  bw->used -= 32;
}

static void bw_put(vp8l_bw_t * bw, uint32_t bits, int n) {
  if (n <= 0 || bw->error) {
    return;
  }
  if (bw->used >= 32) {
    bw_flush32(bw);
  }
  bw->bits |= (uint64_t)bits << bw->used;
  bw->used += n;
}

static int bw_finish(vp8l_bw_t * bw) {
  while (bw->used > 0) {
    if (!bw_grow(bw, 1u)) {
      return 0;
    }
    bw->buf[bw->size++] = (uint8_t)(bw->bits & 0xffu);
    bw->bits >>= 8;
    bw->used -= 8;
  }
  bw->used = 0;
  bw->bits = 0;
  return !bw->error;
}

static uint32_t reverse_bits(int num_bits, uint32_t bits) {
  uint32_t retval = 0;
  int i = 0;
  if (num_bits <= 0) {
    return 0;
  }
  while (i < num_bits) {
    i += 4;
    retval |= (uint32_t)k_reversed4[bits & 0xfu]
        << (VP8L_MAX_CODE_LENGTH + 1 - i);
    bits >>= 4;
  }
  return retval >> (VP8L_MAX_CODE_LENGTH + 1 - num_bits);
}

static void depths_to_codes(huff_code_t * tree) {
  int depth_count[VP8L_MAX_CODE_LENGTH + 1];
  uint32_t next_code[VP8L_MAX_CODE_LENGTH + 1];
  uint32_t code = 0;
  memset(depth_count, 0, sizeof(depth_count));
  for (int i = 0; i < tree->num_symbols; ++i) {
    int L = tree->lengths[i];
    if (L > 0 && L <= VP8L_MAX_CODE_LENGTH) {
      ++depth_count[L];
    }
  }
  depth_count[0] = 0;
  next_code[0] = 0;
  for (int i = 1; i <= VP8L_MAX_CODE_LENGTH; ++i) {
    code = (code + (uint32_t)depth_count[i - 1]) << 1;
    next_code[i] = code;
  }
  for (int i = 0; i < tree->num_symbols; ++i) {
    int L = tree->lengths[i];
    tree->codes[i] =
        L > 0 ? (uint16_t)reverse_bits(L, next_code[L]++) : (uint16_t)0;
  }
}

/**
 * Package-merge style depth assignment is heavy; use a Huffman tree with an
 * iterative depth-limit fix (boost rare counts and rebuild).
 */
static int build_lengths(const uint32_t * histo_in, int n, int depth_limit,
    uint8_t * lengths) {
  uint32_t histo[VP8L_MAX_ALPHABET];
  int symbols[VP8L_MAX_ALPHABET];
  int nsym = 0;

  if (n > VP8L_MAX_ALPHABET) {
    return 0;
  }
  memcpy(histo, histo_in, (size_t)n * sizeof(uint32_t));
  memset(lengths, 0, (size_t)n);

  for (int attempt = 0; attempt < 6; ++attempt) {
    typedef struct {
      uint32_t count;
      int left;
      int right;
      int sym; /* >=0 leaf */
    } node_t;
    node_t nodes[2 * VP8L_MAX_ALPHABET];
    int nnodes = 0;
    int alive[2 * VP8L_MAX_ALPHABET];
    int nalive = 0;
    int root;
    int depth[2 * VP8L_MAX_ALPHABET];
    int too_deep = 0;

    nsym = 0;
    for (int i = 0; i < n; ++i) {
      if (histo[i] > 0u) {
        symbols[nsym++] = i;
      }
    }
    if (nsym == 0) {
      return 1;
    }
    if (nsym == 1) {
      lengths[symbols[0]] = 1;
      return 1;
    }

    for (int i = 0; i < nsym; ++i) {
      int s = symbols[i];
      nodes[nnodes].count = histo[s];
      nodes[nnodes].left = -1;
      nodes[nnodes].right = -1;
      nodes[nnodes].sym = s;
      alive[nalive++] = nnodes;
      ++nnodes;
    }

    while (nalive > 1) {
      int ia = 0;
      int ib = 1;
      int na;
      int nb;
      int tmp_alive[2 * VP8L_MAX_ALPHABET];
      int nn = 0;
      if (nodes[alive[ib]].count < nodes[alive[ia]].count) {
        int t = ia;
        ia = ib;
        ib = t;
      }
      for (int i = 2; i < nalive; ++i) {
        if (nodes[alive[i]].count < nodes[alive[ia]].count) {
          ib = ia;
          ia = i;
        }
        else if (nodes[alive[i]].count < nodes[alive[ib]].count) {
          ib = i;
        }
      }
      na = alive[ia];
      nb = alive[ib];
      for (int i = 0; i < nalive; ++i) {
        if (alive[i] != na && alive[i] != nb) {
          tmp_alive[nn++] = alive[i];
        }
      }
      nodes[nnodes].count = nodes[na].count + nodes[nb].count;
      nodes[nnodes].left = na;
      nodes[nnodes].right = nb;
      nodes[nnodes].sym = -1;
      tmp_alive[nn++] = nnodes;
      ++nnodes;
      memcpy(alive, tmp_alive, (size_t)nn * sizeof(int));
      nalive = nn;
    }
    root = alive[0];
    memset(depth, 0, sizeof(depth));
    /* DFS for depths */
    {
      int stack[2 * VP8L_MAX_ALPHABET];
      int sp = 0;
      stack[sp++] = root;
      depth[root] = 0;
      while (sp > 0) {
        int cur = stack[--sp];
        if (nodes[cur].sym >= 0) {
          continue;
        }
        if (nodes[cur].left >= 0) {
          depth[nodes[cur].left] = depth[cur] + 1;
          stack[sp++] = nodes[cur].left;
        }
        if (nodes[cur].right >= 0) {
          depth[nodes[cur].right] = depth[cur] + 1;
          stack[sp++] = nodes[cur].right;
        }
      }
    }

    memset(lengths, 0, (size_t)n);
    for (int i = 0; i < nnodes; ++i) {
      if (nodes[i].sym >= 0) {
        int d = depth[i];
        if (d < 1) {
          d = 1;
        }
        if (d > depth_limit) {
          too_deep = 1;
          d = depth_limit;
        }
        lengths[nodes[i].sym] = (uint8_t)d;
      }
    }
    if (!too_deep) {
      return 1;
    }
    for (int i = 0; i < n; ++i) {
      if (histo[i] > 0u && histo[i] < 8u) {
        histo[i] *= 2u;
      }
    }
  }

  /* Fallback: flat 8-bit codes for used symbols (always valid for <=256). */
  for (int i = 0; i < n; ++i) {
    lengths[i] = histo_in[i] > 0u ? 8 : 0;
  }
  if (nsym == 1) {
    lengths[symbols[0]] = 1;
  }
  return 1;
}

static int count_tokens(const huff_code_t * tree, huff_token_t * tokens,
    int max_tokens) {
  int prev = 8;
  int i = 0;
  int ntok = 0;
  while (i < tree->num_symbols) {
    int value = tree->lengths[i];
    int k = i + 1;
    int runs;
    while (k < tree->num_symbols && tree->lengths[k] == value) {
      ++k;
    }
    runs = k - i;
    if (value == 0) {
      while (runs >= 1) {
        if (runs < 3) {
          for (int r = 0; r < runs; ++r) {
            if (ntok >= max_tokens) {
              return -1;
            }
            tokens[ntok].code = 0;
            tokens[ntok].extra = 0;
            ++ntok;
          }
          break;
        }
        if (runs < 11) {
          if (ntok >= max_tokens) {
            return -1;
          }
          tokens[ntok].code = 17;
          tokens[ntok].extra = (uint8_t)(runs - 3);
          ++ntok;
          break;
        }
        if (runs < 139) {
          if (ntok >= max_tokens) {
            return -1;
          }
          tokens[ntok].code = 18;
          tokens[ntok].extra = (uint8_t)(runs - 11);
          ++ntok;
          break;
        }
        if (ntok >= max_tokens) {
          return -1;
        }
        tokens[ntok].code = 18;
        tokens[ntok].extra = 0x7f;
        ++ntok;
        runs -= 138;
      }
    }
    else {
      if (value != prev) {
        if (ntok >= max_tokens) {
          return -1;
        }
        tokens[ntok].code = (uint8_t)value;
        tokens[ntok].extra = 0;
        ++ntok;
        --runs;
        prev = value;
      }
      while (runs >= 1) {
        if (runs < 3) {
          for (int r = 0; r < runs; ++r) {
            if (ntok >= max_tokens) {
              return -1;
            }
            tokens[ntok].code = (uint8_t)value;
            tokens[ntok].extra = 0;
            ++ntok;
          }
          break;
        }
        if (runs < 7) {
          if (ntok >= max_tokens) {
            return -1;
          }
          tokens[ntok].code = 16;
          tokens[ntok].extra = (uint8_t)(runs - 3);
          ++ntok;
          break;
        }
        if (ntok >= max_tokens) {
          return -1;
        }
        tokens[ntok].code = 16;
        tokens[ntok].extra = 3;
        ++ntok;
        runs -= 6;
      }
    }
    i = k;
  }
  return ntok;
}

static void clear_if_one_symbol(huff_code_t * tree) {
  int nz = 0;
  int only = -1;
  for (int i = 0; i < tree->num_symbols; ++i) {
    if (tree->lengths[i]) {
      ++nz;
      only = i;
    }
  }
  if (nz == 1 && only >= 0) {
    tree->codes[only] = 0;
    tree->lengths[only] = 0;
  }
}

static void store_code_length_tree(vp8l_bw_t * bw, const uint8_t * lengths) {
  int codes_to_store = VP8L_NUM_CODE_LENGTH;
  while (codes_to_store > 4 &&
      lengths[k_code_length_order[codes_to_store - 1]] == 0) {
    --codes_to_store;
  }
  bw_put(bw, (uint32_t)(codes_to_store - 4), 4);
  for (int i = 0; i < codes_to_store; ++i) {
    bw_put(bw, lengths[k_code_length_order[i]], 3);
  }
}

static void store_full_huffman(vp8l_bw_t * bw, const huff_code_t * tree,
    huff_token_t * tokens, int max_tokens) {
  uint8_t cl_lengths[VP8L_NUM_CODE_LENGTH];
  uint16_t cl_codes[VP8L_NUM_CODE_LENGTH];
  uint32_t cl_histo[VP8L_NUM_CODE_LENGTH];
  huff_code_t cl_tree;
  int ntok = count_tokens(tree, tokens, max_tokens);

  bw_put(bw, 0, 1);
  if (ntok < 0) {
    bw->error = 1;
    return;
  }
  memset(cl_histo, 0, sizeof(cl_histo));
  for (int i = 0; i < ntok; ++i) {
    ++cl_histo[tokens[i].code];
  }
  memset(cl_lengths, 0, sizeof(cl_lengths));
  if (!build_lengths(cl_histo, VP8L_NUM_CODE_LENGTH, 7, cl_lengths)) {
    bw->error = 1;
    return;
  }
  cl_tree.lengths = cl_lengths;
  cl_tree.codes = cl_codes;
  cl_tree.num_symbols = VP8L_NUM_CODE_LENGTH;
  depths_to_codes(&cl_tree);
  store_code_length_tree(bw, cl_lengths);
  clear_if_one_symbol(&cl_tree);

  bw_put(bw, 0, 1); /* no trimmed length */
  for (int i = 0; i < ntok; ++i) {
    int ix = tokens[i].code;
    bw_put(bw, cl_tree.codes[ix], cl_tree.lengths[ix]);
    if (ix == 16) {
      bw_put(bw, tokens[i].extra, 2);
    }
    else if (ix == 17) {
      bw_put(bw, tokens[i].extra, 3);
    }
    else if (ix == 18) {
      bw_put(bw, tokens[i].extra, 7);
    }
  }
}

static void store_huffman(vp8l_bw_t * bw, const huff_code_t * tree,
    huff_token_t * tokens, int max_tokens) {
  int count = 0;
  int symbols[2] = {0, 0};
  for (int i = 0; i < tree->num_symbols && count < 3; ++i) {
    if (tree->lengths[i] != 0) {
      if (count < 2) {
        symbols[count] = i;
      }
      ++count;
    }
  }
  if (count == 0) {
    bw_put(bw, 0x01u, 4);
  }
  else if (count <= 2 && symbols[0] < 256 && symbols[1] < 256) {
    bw_put(bw, 1, 1);
    bw_put(bw, (uint32_t)(count - 1), 1);
    if (symbols[0] <= 1) {
      bw_put(bw, 0, 1);
      bw_put(bw, (uint32_t)symbols[0], 1);
    }
    else {
      bw_put(bw, 1, 1);
      bw_put(bw, (uint32_t)symbols[0], 8);
    }
    if (count == 2) {
      bw_put(bw, (uint32_t)symbols[1], 8);
    }
  }
  else {
    store_full_huffman(bw, tree, tokens, max_tokens);
  }
}

static void write_symbol(vp8l_bw_t * bw, const huff_code_t * tree, int sym) {
  int depth = tree->lengths[sym];
  if (depth > 0) {
    bw_put(bw, tree->codes[sym], depth);
  }
}

static void apply_subtract_green(uint32_t * argb, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uint32_t p = argb[i];
    uint8_t g = (uint8_t)((p >> 8) & 0xffu);
    uint8_t r = (uint8_t)(((p >> 16) & 0xffu) - g);
    uint8_t b = (uint8_t)((p & 0xffu) - g);
    argb[i] = (p & 0xff00ff00u) | ((uint32_t)r << 16) | (uint32_t)b;
  }
}

GIMG_Result gimg_webp_vp8l_encode(const uint8_t * rgba, uint32_t width,
    uint32_t height, size_t stride, int has_alpha_hint, int exact, int effort,
    const GIMG_Allocator * alloc, unsigned char ** out_bytes,
    size_t * out_size) {
  vp8l_bw_t bw;
  uint32_t * argb = NULL;
  huff_code_t trees[5];
  uint8_t * length_bufs[5] = {NULL, NULL, NULL, NULL, NULL};
  uint16_t * code_bufs[5] = {NULL, NULL, NULL, NULL, NULL};
  huff_token_t * tokens = NULL;
  uint32_t hist[5][VP8L_MAX_ALPHABET];
  GIMG_Result r = GIMG_OK;
  size_t n;
  int width_i;
  int height_i;
  int has_alpha = 0;
  int use_subgreen;

  if (!rgba || !out_bytes || !out_size || width == 0u || height == 0u ||
      width > (uint32_t)VP8L_MAX_DIM || height > (uint32_t)VP8L_MAX_DIM) {
    return GIMG_ERR_UNSUPPORTED;
  }
  *out_bytes = NULL;
  *out_size = 0;
  alloc = gimg_alloc_or_default(alloc);
  if (effort < 0) {
    effort = 0;
  }
  if (effort > 9) {
    effort = 9;
  }
  use_subgreen = (effort >= 1);

  width_i = (int)width;
  height_i = (int)height;
  n = (size_t)width_i * (size_t)height_i;
  argb = (uint32_t *)gimg_malloc(alloc, n * sizeof(uint32_t));
  if (!argb) {
    return GIMG_ERR_OOM;
  }

  for (int y = 0; y < height_i; ++y) {
    const uint8_t * row = rgba + (size_t)y * stride;
    for (int x = 0; x < width_i; ++x) {
      const uint8_t * px = row + (size_t)x * 4u;
      uint8_t R = px[0];
      uint8_t G = px[1];
      uint8_t B = px[2];
      uint8_t A = px[3];
      if (A != 255u) {
        has_alpha = 1;
      }
      if (!exact && A == 0u) {
        R = G = B = 0;
      }
      argb[(size_t)y * (size_t)width_i + (size_t)x] =
          ((uint32_t)A << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8) |
          (uint32_t)B;
    }
  }
  if (has_alpha_hint) {
    has_alpha = 1;
  }

  memset(&bw, 0, sizeof(bw));
  bw.alloc = alloc;
  if (!bw_grow(&bw, 64u)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }

  bw_put(&bw, VP8L_MAGIC, 8);
  bw_put(&bw, (uint32_t)(width_i - 1), 14);
  bw_put(&bw, (uint32_t)(height_i - 1), 14);
  bw_put(&bw, has_alpha ? 1u : 0u, 1);
  bw_put(&bw, 0, 3);

  if (use_subgreen) {
    bw_put(&bw, 1, 1);
    bw_put(&bw, (uint32_t)GIMG_WEBP_VP8L_SUBTRACT_GREEN, 2);
    apply_subtract_green(argb, n);
  }
  bw_put(&bw, 0, 1); /* end transforms */
  bw_put(&bw, 0, 1); /* no color cache */
  bw_put(&bw, 0, 1); /* no Huffman image */

  memset(hist, 0, sizeof(hist));
  for (size_t i = 0; i < n; ++i) {
    uint32_t p = argb[i];
    ++hist[VP8L_GREEN][(p >> 8) & 0xffu];
    ++hist[VP8L_RED][(p >> 16) & 0xffu];
    ++hist[VP8L_BLUE][p & 0xffu];
    ++hist[VP8L_ALPHA][(p >> 24) & 0xffu];
  }
  hist[VP8L_DIST][0] = 1u;

  tokens = (huff_token_t *)gimg_malloc(
      alloc, (size_t)VP8L_MAX_ALPHABET * sizeof(huff_token_t));
  if (!tokens) {
    r = GIMG_ERR_OOM;
    goto Done;
  }

  for (int t = 0; t < 5; ++t) {
    int alph = (t == 0) ? VP8L_LIT_ALPHABET
                        : (t == 4 ? VP8L_NUM_DISTANCE : VP8L_NUM_LITERAL);
    length_bufs[t] = (uint8_t *)gimg_calloc(alloc, (size_t)alph, 1u);
    code_bufs[t] =
        (uint16_t *)gimg_calloc(alloc, (size_t)alph, sizeof(uint16_t));
    if (!length_bufs[t] || !code_bufs[t]) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    if (!build_lengths(hist[t], alph, 15, length_bufs[t])) {
      r = GIMG_ERR_INTERNAL;
      goto Done;
    }
    trees[t].lengths = length_bufs[t];
    trees[t].codes = code_bufs[t];
    trees[t].num_symbols = alph;
    depths_to_codes(&trees[t]);
    store_huffman(&bw, &trees[t], tokens, VP8L_MAX_ALPHABET);
    clear_if_one_symbol(&trees[t]);
  }

  for (size_t i = 0; i < n; ++i) {
    uint32_t p = argb[i];
    write_symbol(&bw, &trees[VP8L_GREEN], (int)((p >> 8) & 0xffu));
    write_symbol(&bw, &trees[VP8L_RED], (int)((p >> 16) & 0xffu));
    write_symbol(&bw, &trees[VP8L_BLUE], (int)(p & 0xffu));
    write_symbol(&bw, &trees[VP8L_ALPHA], (int)((p >> 24) & 0xffu));
  }

  if (!bw_finish(&bw) || bw.error) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  *out_bytes = bw.buf;
  *out_size = bw.size;
  bw.buf = NULL;

Done:
  gimg_free(alloc, bw.buf);
  gimg_free(alloc, argb);
  for (int t = 0; t < 5; ++t) {
    gimg_free(alloc, length_bufs[t]);
    gimg_free(alloc, code_bufs[t]);
  }
  gimg_free(alloc, tokens);
  return r;
}
