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
 * VP8L lossless bitstream encode. Effort >= 1 applies subtract-green.
 * Effort >= 2 also applies one spatial predictor when it shrinks the
 * residual, then LZ77 (exact pixel copies, minimum length 4) before
 * Huffman. Effort >= 3 adds one cross-colour transform when it shrinks
 * the red and blue residuals. Round-trip through our decoder is identity.
 * Output is accepted by libwebp's dwebp.
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
  VP8L_MAX_ALPHABET = VP8L_LIT_ALPHABET,
  VP8L_MAX_LENGTH = 4096,
  VP8L_MAX_PLANE = 1048576,
  VP8L_LZ_MIN = 4,
  VP8L_LZ_HASH_BITS = 12,
  VP8L_LZ_CHAIN = 16
};

static const uint32_t k_vp8l_black = 0xff000000u;

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

/* Component-wise subtract, mod 256. Inverse of the decoder's add. */
static uint32_t sub_pixels(uint32_t a, uint32_t b) {
  const uint32_t ag =
      0x00ff00ffu + (a & 0xff00ff00u) - (b & 0xff00ff00u);
  const uint32_t rb =
      0xff00ff00u + (a & 0x00ff00ffu) - (b & 0x00ff00ffu);
  return (ag & 0xff00ff00u) | (rb & 0x00ff00ffu);
}

static uint32_t pred_average2(uint32_t a0, uint32_t a1) {
  return (((a0 ^ a1) & 0xfefefefeu) >> 1) + (a0 & a1);
}

static uint32_t pred_average3(uint32_t a0, uint32_t a1, uint32_t a2) {
  return pred_average2(pred_average2(a0, a2), a1);
}

static uint32_t pred_average4(uint32_t a0, uint32_t a1, uint32_t a2,
    uint32_t a3) {
  return pred_average2(pred_average2(a0, a1), pred_average2(a2, a3));
}

static uint32_t pred_clip255(uint32_t a) {
  if (a < 256u) {
    return a;
  }
  return ~a >> 24;
}

static int pred_add_sub_full(int a, int b, int c) {
  return (int)pred_clip255((uint32_t)(a + b - c));
}

static uint32_t pred_clamped_full(uint32_t c0, uint32_t c1, uint32_t c2) {
  const int a = pred_add_sub_full(
      (int)(c0 >> 24), (int)(c1 >> 24), (int)(c2 >> 24));
  const int r = pred_add_sub_full(
      (int)((c0 >> 16) & 0xff), (int)((c1 >> 16) & 0xff),
      (int)((c2 >> 16) & 0xff));
  const int g = pred_add_sub_full(
      (int)((c0 >> 8) & 0xff), (int)((c1 >> 8) & 0xff),
      (int)((c2 >> 8) & 0xff));
  const int b = pred_add_sub_full(
      (int)(c0 & 0xff), (int)(c1 & 0xff), (int)(c2 & 0xff));
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
      (uint32_t)b;
}

static int pred_add_sub_half(int a, int b) {
  return (int)pred_clip255((uint32_t)(a + (a - b) / 2));
}

static uint32_t pred_clamped_half(uint32_t c0, uint32_t c1, uint32_t c2) {
  const uint32_t ave = pred_average2(c0, c1);
  const int a = pred_add_sub_half((int)(ave >> 24), (int)(c2 >> 24));
  const int r = pred_add_sub_half(
      (int)((ave >> 16) & 0xff), (int)((c2 >> 16) & 0xff));
  const int g = pred_add_sub_half(
      (int)((ave >> 8) & 0xff), (int)((c2 >> 8) & 0xff));
  const int b = pred_add_sub_half((int)(ave & 0xff), (int)(c2 & 0xff));
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
      (uint32_t)b;
}

static int pred_sub3(int a, int b, int c) {
  const int pb = b - c;
  const int pa = a - c;
  const int apb = pb < 0 ? -pb : pb;
  const int apa = pa < 0 ? -pa : pa;
  return apb - apa;
}

static uint32_t pred_select(uint32_t a, uint32_t b, uint32_t c) {
  const int pa_minus_pb =
      pred_sub3((int)(a >> 24), (int)(b >> 24), (int)(c >> 24)) +
      pred_sub3((int)((a >> 16) & 0xff), (int)((b >> 16) & 0xff),
          (int)((c >> 16) & 0xff)) +
      pred_sub3((int)((a >> 8) & 0xff), (int)((b >> 8) & 0xff),
          (int)((c >> 8) & 0xff)) +
      pred_sub3((int)(a & 0xff), (int)(b & 0xff), (int)(c & 0xff));
  return (pa_minus_pb <= 0) ? a : b;
}

/**
 * Spatial predictor @a mode. @a left is the pixel to the left; @a top points
 * at the pixel above, so top[-1] / top[0] / top[1] match the decoder.
 * Row 0 and column 0 are special-cased by the caller, as in predictor_inverse.
 */
static uint32_t predict_mode(int mode, const uint32_t * left,
    const uint32_t * top) {
  switch (mode) {
  case 1:
    return *left;
  case 2:
    return top[0];
  case 3:
    return top[1];
  case 4:
    return top[-1];
  case 5:
    return pred_average3(*left, top[0], top[1]);
  case 6:
    return pred_average2(*left, top[-1]);
  case 7:
    return pred_average2(*left, top[0]);
  case 8:
    return pred_average2(top[-1], top[0]);
  case 9:
    return pred_average2(top[0], top[1]);
  case 10:
    return pred_average4(*left, top[-1], top[0], top[1]);
  case 11:
    return pred_select(top[0], *left, top[-1]);
  case 12:
    return pred_clamped_full(*left, top[0], top[-1]);
  case 13:
    return pred_clamped_half(*left, top[0], top[-1]);
  default:
    return k_vp8l_black;
  }
}

static void residual_image(const uint32_t * src, uint32_t * dst, int width,
    int height, int mode) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int i = y * width + x;
      uint32_t pred;
      if (y == 0) {
        pred = (x == 0) ? k_vp8l_black : src[i - 1];
      }
      else if (x == 0) {
        pred = src[i - width];
      }
      else {
        pred = predict_mode(mode, &src[i - 1], &src[i - width]);
      }
      dst[i] = sub_pixels(src[i], pred);
    }
  }
}

static uint64_t residual_sad(const uint32_t * p, size_t n) {
  uint64_t s = 0;
  for (size_t i = 0; i < n; ++i) {
    uint32_t v = p[i];
    for (int k = 0; k < 4; ++k) {
      int b = (int)((v >> (8 * k)) & 0xffu);
      s += (uint64_t)((b >= 128) ? (256 - b) : b);
    }
  }
  return s;
}

static int covering_bits(int width, int height) {
  for (int bits = 9; bits >= 2; --bits) {
    const int tile = 1 << bits;
    const int tw = (width + tile - 1) >> bits;
    const int th = (height + tile - 1) >> bits;
    if (tw == 1 && th == 1) {
      return bits;
    }
  }
  return 9;
}

static int prefix_of(int value, int * sym, int * nextra, int * extra) {
  if (value < 1) {
    return 0;
  }
  if (value <= 4) {
    *sym = value - 1;
    *nextra = 0;
    *extra = 0;
    return 1;
  }
  for (int s = 4; s < VP8L_NUM_DISTANCE; ++s) {
    const int eb = (s - 2) >> 1;
    const int offset = (2 + (s & 1)) << eb;
    const int base = offset + 1;
    const int count = 1 << eb;
    if (value >= base && value < base + count) {
      *sym = s;
      *nextra = eb;
      *extra = value - base;
      return 1;
    }
  }
  return 0;
}

typedef struct {
  int pass;
  int error;
  uint32_t hist[5][VP8L_MAX_ALPHABET];
  vp8l_bw_t * bw;
  huff_code_t * trees;
} walk_ctx_t;

static void count_literal(walk_ctx_t * ctx, uint32_t p) {
  ++ctx->hist[VP8L_GREEN][(p >> 8) & 0xffu];
  ++ctx->hist[VP8L_RED][(p >> 16) & 0xffu];
  ++ctx->hist[VP8L_BLUE][p & 0xffu];
  ++ctx->hist[VP8L_ALPHA][(p >> 24) & 0xffu];
}

static void emit_token(walk_ctx_t * ctx, int is_copy, uint32_t pix, int len,
    int dist) {
  int ls = 0;
  int le = 0;
  int lx = 0;
  int ds = 0;
  int de = 0;
  int dx = 0;
  if (ctx->error) {
    return;
  }
  if (!is_copy) {
    if (ctx->pass == 0) {
      count_literal(ctx, pix);
    }
    else {
      write_symbol(ctx->bw, &ctx->trees[VP8L_GREEN], (int)((pix >> 8) & 0xffu));
      write_symbol(ctx->bw, &ctx->trees[VP8L_RED], (int)((pix >> 16) & 0xffu));
      write_symbol(ctx->bw, &ctx->trees[VP8L_BLUE], (int)(pix & 0xffu));
      write_symbol(ctx->bw, &ctx->trees[VP8L_ALPHA], (int)((pix >> 24) & 0xffu));
    }
    return;
  }
  if (!prefix_of(len, &ls, &le, &lx) || ls >= VP8L_NUM_LENGTH ||
      !prefix_of(dist + 120, &ds, &de, &dx)) {
    ctx->error = 1;
    return;
  }
  if (ctx->pass == 0) {
    ++ctx->hist[VP8L_GREEN][VP8L_NUM_LITERAL + ls];
    ++ctx->hist[VP8L_DIST][ds];
  }
  else {
    write_symbol(ctx->bw, &ctx->trees[VP8L_GREEN], VP8L_NUM_LITERAL + ls);
    bw_put(ctx->bw, (uint32_t)lx, le);
    write_symbol(ctx->bw, &ctx->trees[VP8L_DIST], ds);
    bw_put(ctx->bw, (uint32_t)dx, de);
  }
}

static int pix_hash(uint32_t p) {
  return (int)((p * 2654435761u) >> (32 - VP8L_LZ_HASH_BITS));
}

static void consider_dist(const uint32_t * pix, int i, int n, int dist,
    int * best_len, int * best_dist) {
  int len = 0;
  int maxl;
  if (dist <= 0 || dist > i || dist + 120 > VP8L_MAX_PLANE) {
    return;
  }
  if (pix[i] != pix[i - dist]) {
    return;
  }
  maxl = n - i;
  if (maxl > VP8L_MAX_LENGTH) {
    maxl = VP8L_MAX_LENGTH;
  }
  while (len < maxl && pix[i + len] == pix[i + len - dist]) {
    ++len;
  }
  if (len > *best_len || (len == *best_len && dist < *best_dist)) {
    *best_len = len;
    *best_dist = dist;
  }
}

static void lz_insert(int * head, int * prev, const uint32_t * pix, int i) {
  const int h = pix_hash(pix[i]);
  prev[i] = head[h];
  head[h] = i;
}

static int walk_pixels(const uint32_t * pix, int width, int height,
    int use_lz77, const GIMG_Allocator * alloc, walk_ctx_t * ctx) {
  const int n = width * height;
  int * prev = NULL;
  int head[1 << VP8L_LZ_HASH_BITS];
  int i = 0;

  if (use_lz77) {
    prev = (int *)gimg_malloc(alloc, (size_t)n * sizeof(int));
    if (!prev) {
      return 0;
    }
    for (int h = 0; h < (1 << VP8L_LZ_HASH_BITS); ++h) {
      head[h] = -1;
    }
  }

  while (i < n && !ctx->error) {
    int best_len = 0;
    int best_dist = 0;
    if (use_lz77) {
      int chain = 0;
      int pos;
      consider_dist(pix, i, n, 1, &best_len, &best_dist);
      if (i >= width) {
        consider_dist(pix, i, n, width, &best_len, &best_dist);
      }
      for (pos = head[pix_hash(pix[i])]; pos >= 0 && chain < VP8L_LZ_CHAIN;
           pos = prev[pos], ++chain) {
        consider_dist(pix, i, n, i - pos, &best_len, &best_dist);
      }
      if (best_len >= VP8L_LZ_MIN) {
        emit_token(ctx, 1, 0, best_len, best_dist);
        for (int k = 0; k < best_len; ++k) {
          lz_insert(head, prev, pix, i + k);
        }
        i += best_len;
        continue;
      }
    }
    emit_token(ctx, 0, pix[i], 0, 0);
    if (use_lz77) {
      lz_insert(head, prev, pix, i);
    }
    ++i;
  }
  gimg_free(alloc, prev);
  return !ctx->error;
}

/**
 * One VP8L image body: optional Huffman-image bit (level 0 only), five
 * Huffman trees, then pixels. @a use_lz77 selects backward references.
 */
static GIMG_Result write_coded_image(vp8l_bw_t * bw, const uint32_t * argb,
    int width, int height, int write_meta_bit, int use_lz77,
    const GIMG_Allocator * alloc) {
  walk_ctx_t ctx;
  huff_code_t trees[5];
  uint8_t * length_bufs[5] = {NULL, NULL, NULL, NULL, NULL};
  uint16_t * code_bufs[5] = {NULL, NULL, NULL, NULL, NULL};
  huff_token_t * tokens = NULL;
  GIMG_Result r = GIMG_OK;
  int dist_used = 0;

  memset(&ctx, 0, sizeof(ctx));
  ctx.bw = bw;
  ctx.trees = trees;
  bw_put(bw, 0, 1);
  if (write_meta_bit) {
    bw_put(bw, 0, 1);
  }
  if (!walk_pixels(argb, width, height, use_lz77, alloc, &ctx)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  if (ctx.error) {
    r = GIMG_ERR_INTERNAL;
    goto Done;
  }
  for (int i = 0; i < VP8L_NUM_DISTANCE; ++i) {
    if (ctx.hist[VP8L_DIST][i] != 0u) {
      dist_used = 1;
      break;
    }
  }
  if (!dist_used) {
    ctx.hist[VP8L_DIST][0] = 1u;
  }

  tokens = (huff_token_t *)gimg_malloc(
      alloc, (size_t)VP8L_MAX_ALPHABET * sizeof(huff_token_t));
  if (!tokens) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  for (int t = 0; t < 5; ++t) {
    const int alph = (t == 0) ? VP8L_LIT_ALPHABET
                              : (t == 4 ? VP8L_NUM_DISTANCE : VP8L_NUM_LITERAL);
    length_bufs[t] = (uint8_t *)gimg_calloc(alloc, (size_t)alph, 1u);
    code_bufs[t] =
        (uint16_t *)gimg_calloc(alloc, (size_t)alph, sizeof(uint16_t));
    if (!length_bufs[t] || !code_bufs[t]) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    if (!build_lengths(ctx.hist[t], alph, 15, length_bufs[t])) {
      r = GIMG_ERR_INTERNAL;
      goto Done;
    }
    trees[t].lengths = length_bufs[t];
    trees[t].codes = code_bufs[t];
    trees[t].num_symbols = alph;
    depths_to_codes(&trees[t]);
    store_huffman(bw, &trees[t], tokens, VP8L_MAX_ALPHABET);
    clear_if_one_symbol(&trees[t]);
  }
  if (bw->error) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  ctx.pass = 1;
  if (!walk_pixels(argb, width, height, use_lz77, alloc, &ctx) || ctx.error ||
      bw->error) {
    r = ctx.error ? GIMG_ERR_INTERNAL : GIMG_ERR_OOM;
  }

Done:
  for (int t = 0; t < 5; ++t) {
    gimg_free(alloc, length_bufs[t]);
    gimg_free(alloc, code_bufs[t]);
  }
  gimg_free(alloc, tokens);
  return r;
}

static int clamp_i8(int v) {
  if (v < -128) {
    return -128;
  }
  if (v > 127) {
    return 127;
  }
  return v;
}

static int round_ratio(double num, double den) {
  double q;
  if (den == 0.0) {
    return 0;
  }
  q = num / den;
  if (q >= 0.0) {
    return (int)(q + 0.5);
  }
  return (int)(q - 0.5);
}

static int color_delta(int8_t pred, int8_t color) {
  return ((int)pred * (int)color) >> 5;
}

/**
 * Least-squares multipliers for one tile. (m * channel) >> 5 approximates
 * the channel it predicts, which is the inverse the decoder adds back.
 */
static void fit_cross_rect(const uint32_t * pix, int width, int x0, int y0,
    int x1, int y1, int * g2r, int * g2b, int * r2b) {
  double sgg = 0.0;
  double sgr = 0.0;
  double sgb = 0.0;
  double srr = 0.0;
  double srb = 0.0;
  double det;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const uint32_t p = pix[(size_t)y * (size_t)width + (size_t)x];
      const double g = (double)(int8_t)(p >> 8);
      const double r = (double)(int8_t)(p >> 16);
      const double b = (double)(int8_t)p;
      sgg += g * g;
      sgr += g * r;
      sgb += g * b;
      srr += r * r;
      srb += r * b;
    }
  }
  *g2r = clamp_i8(round_ratio(32.0 * sgr, sgg));
  det = sgg * srr - sgr * sgr;
  if (det == 0.0) {
    *g2b = clamp_i8(round_ratio(32.0 * sgb, sgg));
    *r2b = 0;
  }
  else {
    *g2b = clamp_i8(round_ratio(32.0 * (sgb * srr - sgr * srb), det));
    *r2b = clamp_i8(round_ratio(32.0 * (sgg * srb - sgr * sgb), det));
  }
}

static uint32_t pack_cross(int g2r, int g2b, int r2b) {
  return ((uint32_t)(uint8_t)r2b << 16) | ((uint32_t)(uint8_t)g2b << 8) |
      (uint32_t)(uint8_t)g2r;
}

static void apply_cross_tiled(uint32_t * pix, int width, int height, int bits,
    const uint32_t * codes) {
  const int tile = 1 << bits;
  const int tw = (width + tile - 1) >> bits;
  for (int y = 0; y < height; ++y) {
    const uint32_t * row = codes + (size_t)(y >> bits) * (size_t)tw;
    int x = 0;
    while (x < width) {
      const uint32_t code = row[x >> bits];
      const int g2r = (int8_t)(code & 0xffu);
      const int g2b = (int8_t)((code >> 8) & 0xffu);
      const int r2b = (int8_t)((code >> 16) & 0xffu);
      int x1 = (x & ~(tile - 1)) + tile;
      if (x1 > width) {
        x1 = width;
      }
      for (; x < x1; ++x) {
        const size_t i = (size_t)y * (size_t)width + (size_t)x;
        const uint32_t argb = pix[i];
        const int8_t green = (int8_t)(argb >> 8);
        const int8_t red = (int8_t)(argb >> 16);
        int new_red = (int)((argb >> 16) & 0xffu);
        int new_blue = (int)(argb & 0xffu);
        new_red -= color_delta((int8_t)g2r, green);
        new_red &= 0xff;
        new_blue -= color_delta((int8_t)g2b, green);
        new_blue -= color_delta((int8_t)r2b, red);
        new_blue &= 0xff;
        pix[i] = (argb & 0xff00ff00u) | ((uint32_t)new_red << 16) |
            (uint32_t)new_blue;
      }
    }
  }
}

static void fill_cross_tiles(const uint32_t * pix, int width, int height,
    int bits, uint32_t * codes) {
  const int tile = 1 << bits;
  const int tw = (width + tile - 1) >> bits;
  const int th = (height + tile - 1) >> bits;
  for (int ty = 0; ty < th; ++ty) {
    for (int tx = 0; tx < tw; ++tx) {
      int g2r = 0;
      int g2b = 0;
      int r2b = 0;
      int x1 = (tx + 1) * tile;
      int y1 = (ty + 1) * tile;
      if (x1 > width) {
        x1 = width;
      }
      if (y1 > height) {
        y1 = height;
      }
      fit_cross_rect(pix, width, tx * tile, ty * tile, x1, y1, &g2r, &g2b,
          &r2b);
      codes[(size_t)ty * (size_t)tw + (size_t)tx] = pack_cross(g2r, g2b, r2b);
    }
  }
}

GIMG_Result gimg_webp_vp8l_encode(const uint8_t * rgba, uint32_t width,
    uint32_t height, size_t stride, int has_alpha_hint, int exact, int effort,
    const GIMG_Allocator * alloc, unsigned char ** out_bytes,
    size_t * out_size) {
  vp8l_bw_t bw;
  uint32_t * argb = NULL;
  uint32_t * resid = NULL;
  uint32_t * modes = NULL;
  uint32_t * cc_pix = NULL;
  uint32_t * trial = NULL;
  GIMG_Result r = GIMG_OK;
  size_t n;
  int width_i;
  int height_i;
  int has_alpha = 0;
  int use_subgreen;
  int use_pred = 0;
  int use_lz77;
  int pred_mode = 0;
  int pred_bits = 0;
  int use_cross = 0;
  int cc_bits = 0;

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
  use_lz77 = (effort >= 2);

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
    apply_subtract_green(argb, n);
  }
  if (effort >= 2 && width_i >= 2 && height_i >= 2) {
    uint64_t best = residual_sad(argb, n);
    resid = (uint32_t *)gimg_malloc(alloc, n * sizeof(uint32_t));
    if (!resid) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    for (int mode = 0; mode < 14; ++mode) {
      uint64_t score;
      residual_image(argb, resid, width_i, height_i, mode);
      score = residual_sad(resid, n);
      if (score < best) {
        best = score;
        pred_mode = mode;
        use_pred = 1;
      }
    }
    if (use_pred) {
      residual_image(argb, resid, width_i, height_i, pred_mode);
      pred_bits = covering_bits(width_i, height_i);
    }
  }

  if (use_subgreen) {
    bw_put(&bw, 1, 1);
    bw_put(&bw, (uint32_t)GIMG_WEBP_VP8L_SUBTRACT_GREEN, 2);
  }
  if (use_pred) {
    const int tile = 1 << pred_bits;
    const int tw = (width_i + tile - 1) >> pred_bits;
    const int th = (height_i + tile - 1) >> pred_bits;
    const uint32_t mode_pix =
        0xff000000u | ((uint32_t)pred_mode << 8);
    bw_put(&bw, 1, 1);
    bw_put(&bw, (uint32_t)GIMG_WEBP_VP8L_PREDICTOR, 2);
    bw_put(&bw, (uint32_t)(pred_bits - 2), 3);
    modes = (uint32_t *)gimg_malloc(alloc, (size_t)tw * (size_t)th * sizeof(uint32_t));
    if (!modes) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    for (int i = 0; i < tw * th; ++i) {
      modes[i] = mode_pix;
    }
    r = write_coded_image(&bw, modes, tw, th, 0, 0, alloc);
    if (r != GIMG_OK) {
      goto Done;
    }
  }
  {
    uint32_t * const coded = use_pred ? resid : argb;
    /* Coarsest first. A finer grid replaces it only when residual SAD drops,
     * so a tie keeps the smaller coefficient image. */
    int try_bits[4];
    int ntry = 0;
    const int cover = covering_bits(width_i, height_i);
    uint64_t best_sad = residual_sad(coded, n);
    int best_bits = 0;
    try_bits[ntry++] = cover;
    for (int bits = 5; bits >= 3; --bits) {
      if (bits != cover) {
        try_bits[ntry++] = bits;
      }
    }
    if (effort >= 3 && n >= 2u) {
      trial = (uint32_t *)gimg_malloc(alloc, n * sizeof(uint32_t));
      if (!trial) {
        r = GIMG_ERR_OOM;
        goto Done;
      }
      for (int bi = 0; bi < ntry; ++bi) {
        const int bits = try_bits[bi];
        const int tile = 1 << bits;
        const int tw = (width_i + tile - 1) >> bits;
        const int th = (height_i + tile - 1) >> bits;
        uint32_t * codes;
        uint64_t sad;
        if (bits < 2 || bits > 9) {
          continue;
        }
        codes = (uint32_t *)gimg_malloc(
            alloc, (size_t)tw * (size_t)th * sizeof(uint32_t));
        if (!codes) {
          r = GIMG_ERR_OOM;
          goto Done;
        }
        fill_cross_tiles(coded, width_i, height_i, bits, codes);
        memcpy(trial, coded, n * sizeof(uint32_t));
        apply_cross_tiled(trial, width_i, height_i, bits, codes);
        sad = residual_sad(trial, n);
        if (sad < best_sad) {
          best_sad = sad;
          best_bits = bits;
          gimg_free(alloc, cc_pix);
          cc_pix = codes;
        }
        else {
          gimg_free(alloc, codes);
        }
      }
      if (best_bits != 0) {
        memcpy(trial, coded, n * sizeof(uint32_t));
        apply_cross_tiled(trial, width_i, height_i, best_bits, cc_pix);
        memcpy(coded, trial, n * sizeof(uint32_t));
        use_cross = 1;
        cc_bits = best_bits;
      }
    }
  }
  if (use_cross) {
    const int tile = 1 << cc_bits;
    const int tw = (width_i + tile - 1) >> cc_bits;
    const int th = (height_i + tile - 1) >> cc_bits;
    bw_put(&bw, 1, 1);
    bw_put(&bw, (uint32_t)GIMG_WEBP_VP8L_CROSS_COLOR, 2);
    bw_put(&bw, (uint32_t)(cc_bits - 2), 3);
    r = write_coded_image(&bw, cc_pix, tw, th, 0, 0, alloc);
    if (r != GIMG_OK) {
      goto Done;
    }
  }
  bw_put(&bw, 0, 1); /* end transforms */

  r = write_coded_image(
      &bw, use_pred ? resid : argb, width_i, height_i, 1, use_lz77, alloc);
  if (r != GIMG_OK) {
    goto Done;
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
  gimg_free(alloc, resid);
  gimg_free(alloc, modes);
  gimg_free(alloc, cc_pix);
  gimg_free(alloc, trial);
  return r;
}
