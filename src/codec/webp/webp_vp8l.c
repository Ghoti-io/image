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
 * VP8L lossless bitstream decode. Behaviour matches libwebp 1.5.0
 * (vp8l_dec.c / format_constants.h / color_cache_utils.h).
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "webp_internal.h"

enum {
  VP8L_MAGIC = 0x2f,
  VP8L_MAX_CACHE_BITS = 11,
  VP8L_NUM_LITERAL = 256,
  VP8L_NUM_LENGTH = 24,
  VP8L_NUM_DISTANCE = 40,
  VP8L_NUM_CODE_LENGTH = 19,
  VP8L_MAX_CODE_LENGTH = 15,
  VP8L_HUFFMAN_PER_META = 5,
  VP8L_NUM_TRANSFORMS = 4,
  VP8L_GREEN = 0,
  VP8L_RED = 1,
  VP8L_BLUE = 2,
  VP8L_ALPHA = 3,
  VP8L_DIST = 4,
  VP8L_HUFF_NODE_POOL = 65536,
  VP8L_MAX_SYMBOLS = VP8L_NUM_LITERAL + VP8L_NUM_LENGTH + (1 << VP8L_MAX_CACHE_BITS)
};

static const uint16_t k_alphabet_size[VP8L_HUFFMAN_PER_META] = {
  VP8L_NUM_LITERAL + VP8L_NUM_LENGTH, VP8L_NUM_LITERAL, VP8L_NUM_LITERAL,
  VP8L_NUM_LITERAL, VP8L_NUM_DISTANCE
};

static const uint8_t k_code_length_order[VP8L_NUM_CODE_LENGTH] = {
  17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const uint8_t k_code_length_extra_bits[3] = {2, 3, 7};
static const uint8_t k_code_length_repeat_offsets[3] = {3, 3, 11};

static const uint8_t k_code_to_plane[120] = {
  0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a,
  0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04,
  0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45,
  0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d,
  0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
  0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
  0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e,
  0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e,
  0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d,
  0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70
};

static uint32_t vp8l_subsample_size(uint32_t size, uint32_t sampling_bits) {
  return (size + (1u << sampling_bits) - 1u) >> sampling_bits;
}

/* -------------------------------------------------------------------------- */
/* Bit reader (LSB-first within each byte)                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
  const unsigned char * buf;
  size_t size;
  size_t byte_pos;
  uint64_t bits;
  int nbits;
  int eos;
} vp8l_br_t;

static void br_init(vp8l_br_t * br, const unsigned char * data, size_t size) {
  memset(br, 0, sizeof(*br));
  br->buf = data;
  br->size = size;
}

static void br_fill(vp8l_br_t * br) {
  while (br->nbits <= 56) {
    if (br->byte_pos >= br->size) {
      return;
    }
    br->bits |= (uint64_t)br->buf[br->byte_pos++] << br->nbits;
    br->nbits += 8;
  }
}

static int br_read_bits(vp8l_br_t * br, int n) {
  uint32_t val;
  if (n <= 0) {
    return 0;
  }
  if (br->nbits < n) {
    br_fill(br);
  }
  if (br->nbits < n) {
    br->eos = 1;
    return 0;
  }
  val = (uint32_t)(br->bits & ((1ull << n) - 1ull));
  br->bits >>= n;
  br->nbits -= n;
  return (int)val;
}

/* -------------------------------------------------------------------------- */
/* Huffman trees (bit-by-bit walk; codes match libwebp reversed keys)         */
/* -------------------------------------------------------------------------- */

typedef struct {
  /* child[b]: >=0 node index; -1 empty; <=-2 leaf with symbol = -child-2. */
  int32_t child[2];
} vp8l_huff_node_t;

typedef struct {
  vp8l_huff_node_t * nodes;
  int capacity;
  int used;
  int32_t root; /* -1 empty; >=0 node; leaf-only tree uses special */
  int single_symbol; /* >=0 if tree is a single symbol (0 bits) */
  int has_single;
} vp8l_huff_t;

static void huff_clear(vp8l_huff_t * h) {
  h->used = 0;
  h->root = -1;
  h->single_symbol = -1;
  h->has_single = 0;
}

static int huff_new_node(vp8l_huff_t * h) {
  if (h->used >= h->capacity) {
    return -1;
  }
  h->nodes[h->used].child[0] = -1;
  h->nodes[h->used].child[1] = -1;
  return h->used++;
}

static uint32_t get_next_key(uint32_t key, int len) {
  uint32_t step = 1u << (len - 1);
  while (key & step) {
    step >>= 1;
  }
  return step ? (key & (step - 1u)) + step : key;
}

static int huff_insert(vp8l_huff_t * h, uint32_t key, int len, int symbol) {
  int32_t * slot;
  int bit;
  if (h->root < 0) {
    h->root = huff_new_node(h);
    if (h->root < 0) {
      return 0;
    }
  }
  slot = &h->root;
  for (int i = 0; i < len; ++i) {
    int idx;
    if (*slot < 0) {
      return 0; /* leaf where a branch is needed */
    }
    bit = (int)((key >> i) & 1u);
    idx = *slot;
    slot = &h->nodes[idx].child[bit];
    if (i + 1 < len) {
      if (*slot < 0 && *slot != -1) {
        return 0; /* occupied by a shorter code */
      }
      if (*slot == -1) {
        int n = huff_new_node(h);
        if (n < 0) {
          return 0;
        }
        *slot = n;
      }
    }
  }
  if (*slot != -1) {
    return 0;
  }
  *slot = -(symbol + 2);
  return 1;
}

static int huff_build(vp8l_huff_t * h, const int * code_lengths,
    int alphabet_size) {
  int count[VP8L_MAX_CODE_LENGTH + 1];
  int offset[VP8L_MAX_CODE_LENGTH + 1];
  uint16_t sorted[VP8L_MAX_SYMBOLS];
  int symbol;
  int len;
  int num_symbols = 0;

  huff_clear(h);
  memset(count, 0, sizeof(count));
  for (symbol = 0; symbol < alphabet_size; ++symbol) {
    int L = code_lengths[symbol];
    if (L < 0 || L > VP8L_MAX_CODE_LENGTH) {
      return 0;
    }
    ++count[L];
  }
  if (count[0] == alphabet_size) {
    return 0;
  }

  offset[1] = 0;
  for (len = 1; len < VP8L_MAX_CODE_LENGTH; ++len) {
    if (count[len] > (1 << len)) {
      return 0;
    }
    offset[len + 1] = offset[len] + count[len];
  }
  for (symbol = 0; symbol < alphabet_size; ++symbol) {
    int L = code_lengths[symbol];
    if (L > 0) {
      if (offset[L] >= alphabet_size) {
        return 0;
      }
      sorted[offset[L]++] = (uint16_t)symbol;
      ++num_symbols;
    }
  }

  if (num_symbols == 1) {
    h->has_single = 1;
    h->single_symbol = sorted[0];
    return 1;
  }

  {
    uint32_t key = 0;
    int sym_i = 0;
    for (len = 1; len <= VP8L_MAX_CODE_LENGTH; ++len) {
      for (; count[len] > 0; --count[len]) {
        if (!huff_insert(h, key, len, sorted[sym_i++])) {
          return 0;
        }
        key = get_next_key(key, len);
      }
    }
  }
  return 1;
}

static int huff_read_symbol(const vp8l_huff_t * h, vp8l_br_t * br) {
  int32_t node;
  if (h->has_single) {
    return h->single_symbol;
  }
  if (h->root < 0) {
    return -1;
  }
  node = h->root;
  for (;;) {
    int bit;
    int32_t next;
    if (br->nbits < 1) {
      br_fill(br);
    }
    if (br->nbits < 1) {
      br->eos = 1;
      return -1;
    }
    bit = (int)(br->bits & 1u);
    br->bits >>= 1;
    br->nbits -= 1;
    next = h->nodes[node].child[bit];
    if (next <= -2) {
      return -next - 2;
    }
    if (next < 0) {
      return -1;
    }
    node = next;
  }
}

/* -------------------------------------------------------------------------- */
/* Colour cache                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
  uint32_t * colors;
  int hash_shift;
  int hash_bits;
  int size;
} vp8l_cache_t;

static const uint32_t k_hash_mul = 0x1e35a7bdu;

static void cache_insert(vp8l_cache_t * cc, uint32_t argb) {
  const int key = (int)((argb * k_hash_mul) >> cc->hash_shift);
  cc->colors[key] = argb;
}

static uint32_t cache_lookup(const vp8l_cache_t * cc, int key) {
  return cc->colors[key];
}

/* -------------------------------------------------------------------------- */
/* Decoder state                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
  vp8l_huff_t trees[VP8L_HUFFMAN_PER_META];
} vp8l_htree_group_t;

typedef struct {
  const GIMG_Allocator * alloc;
  vp8l_br_t br;
  GIMG_Result err;

  gimg_webp_vp8l_xform_t transforms[VP8L_NUM_TRANSFORMS];
  int next_transform;
  unsigned transforms_seen;

  int width;  /* coding width after transforms */
  int height;
  int canvas_width;
  int canvas_height;

  uint32_t * huffman_image;
  int huffman_xsize;
  int huffman_subsample_bits;
  int huffman_mask;

  vp8l_htree_group_t * htree_groups;
  int num_htree_groups;

  vp8l_huff_node_t * node_pool;
  int node_pool_cap;
  int node_pool_used;

  vp8l_cache_t color_cache;
  int color_cache_size;

  int code_lengths[VP8L_MAX_SYMBOLS];
} vp8l_dec_t;

static void set_err(vp8l_dec_t * dec, GIMG_Result r) {
  if (dec->err == GIMG_OK) {
    dec->err = r;
  }
}

static void clear_metadata(vp8l_dec_t * dec) {
  gimg_free(dec->alloc, dec->huffman_image);
  dec->huffman_image = NULL;
  gimg_free(dec->alloc, dec->htree_groups);
  dec->htree_groups = NULL;
  dec->num_htree_groups = 0;
  dec->node_pool_used = 0;
  if (dec->color_cache.colors) {
    gimg_free(dec->alloc, dec->color_cache.colors);
    dec->color_cache.colors = NULL;
  }
  dec->color_cache_size = 0;
  dec->huffman_subsample_bits = 0;
  dec->huffman_mask = ~0;
  dec->huffman_xsize = 0;
}

static void clear_transforms(vp8l_dec_t * dec) {
  for (int i = 0; i < dec->next_transform; ++i) {
    gimg_free(dec->alloc, dec->transforms[i].data);
    dec->transforms[i].data = NULL;
  }
  dec->next_transform = 0;
  dec->transforms_seen = 0;
}

static int decode_image_stream(vp8l_dec_t * dec, int xsize, int ysize,
    int is_level0, uint32_t ** decoded_data);

static int get_copy_distance(int distance_symbol, vp8l_br_t * br) {
  int extra_bits;
  int offset;
  if (distance_symbol < 4) {
    return distance_symbol + 1;
  }
  extra_bits = (distance_symbol - 2) >> 1;
  offset = (2 + (distance_symbol & 1)) << extra_bits;
  return offset + br_read_bits(br, extra_bits) + 1;
}

static int plane_code_to_distance(int xsize, int plane_code) {
  if (plane_code > 120) {
    return plane_code - 120;
  }
  {
    const int dist_code = k_code_to_plane[plane_code - 1];
    const int yoffset = dist_code >> 4;
    const int xoffset = 8 - (dist_code & 0xf);
    const int dist = yoffset * xsize + xoffset;
    return (dist >= 1) ? dist : 1;
  }
}

static void copy_block(uint32_t * dst, int dist, int length) {
  const uint32_t * src = dst - dist;
  if (dist >= length) {
    memcpy(dst, src, (size_t)length * sizeof(*dst));
  }
  else {
    for (int i = 0; i < length; ++i) {
      dst[i] = src[i];
    }
  }
}

static vp8l_htree_group_t * get_htree_group(vp8l_dec_t * dec, int x, int y) {
  uint32_t pix = 0;
  if (dec->huffman_image) {
    const int meta =
        (y >> dec->huffman_subsample_bits) * dec->huffman_xsize +
        (x >> dec->huffman_subsample_bits);
    pix = dec->huffman_image[meta];
  }
  if ((int)pix >= dec->num_htree_groups) {
    return NULL;
  }
  return &dec->htree_groups[pix];
}

static int read_huffman_code_lengths(vp8l_dec_t * dec,
    const int * code_length_code_lengths, int num_symbols,
    int * code_lengths) {
  vp8l_huff_t length_tree;
  vp8l_huff_node_t local_nodes[64];
  int symbol;
  int max_symbol;
  int prev_code_len = 8;

  memset(&length_tree, 0, sizeof(length_tree));
  length_tree.nodes = local_nodes;
  length_tree.capacity = 64;
  if (!huff_build(
          &length_tree, code_length_code_lengths, VP8L_NUM_CODE_LENGTH)) {
    set_err(dec, GIMG_ERR_CORRUPT);
    return 0;
  }

  if (br_read_bits(&dec->br, 1)) {
    const int length_nbits = 2 + 2 * br_read_bits(&dec->br, 3);
    max_symbol = 2 + br_read_bits(&dec->br, length_nbits);
    if (max_symbol > num_symbols) {
      set_err(dec, GIMG_ERR_CORRUPT);
      return 0;
    }
  }
  else {
    max_symbol = num_symbols;
  }

  symbol = 0;
  while (symbol < num_symbols) {
    int code_len;
    if (max_symbol-- == 0) {
      break;
    }
    code_len = huff_read_symbol(&length_tree, &dec->br);
    if (code_len < 0 || dec->br.eos) {
      set_err(dec, GIMG_ERR_CORRUPT);
      return 0;
    }
    if (code_len < 16) {
      code_lengths[symbol++] = code_len;
      if (code_len != 0) {
        prev_code_len = code_len;
      }
    }
    else {
      const int use_prev = (code_len == 16);
      const int slot = code_len - 16;
      const int extra_bits = k_code_length_extra_bits[slot];
      const int repeat_offset = k_code_length_repeat_offsets[slot];
      int repeat = br_read_bits(&dec->br, extra_bits) + repeat_offset;
      if (symbol + repeat > num_symbols) {
        set_err(dec, GIMG_ERR_CORRUPT);
        return 0;
      }
      {
        const int length = use_prev ? prev_code_len : 0;
        while (repeat-- > 0) {
          code_lengths[symbol++] = length;
        }
      }
    }
  }
  return 1;
}

static int read_huffman_code(vp8l_dec_t * dec, int alphabet_size,
    vp8l_huff_t * out_tree) {
  const int simple_code = br_read_bits(&dec->br, 1);
  memset(dec->code_lengths, 0, (size_t)alphabet_size * sizeof(int));

  if (simple_code) {
    const int num_symbols = br_read_bits(&dec->br, 1) + 1;
    const int first_symbol_len_code = br_read_bits(&dec->br, 1);
    int symbol =
        br_read_bits(&dec->br, (first_symbol_len_code == 0) ? 1 : 8);
    if (symbol >= alphabet_size) {
      set_err(dec, GIMG_ERR_CORRUPT);
      return 0;
    }
    dec->code_lengths[symbol] = 1;
    if (num_symbols == 2) {
      symbol = br_read_bits(&dec->br, 8);
      if (symbol >= alphabet_size) {
        set_err(dec, GIMG_ERR_CORRUPT);
        return 0;
      }
      dec->code_lengths[symbol] = 1;
    }
  }
  else {
    int code_length_code_lengths[VP8L_NUM_CODE_LENGTH];
    const int num_codes = br_read_bits(&dec->br, 4) + 4;
    memset(code_length_code_lengths, 0, sizeof(code_length_code_lengths));
    if (num_codes > VP8L_NUM_CODE_LENGTH) {
      set_err(dec, GIMG_ERR_CORRUPT);
      return 0;
    }
    for (int i = 0; i < num_codes; ++i) {
      code_length_code_lengths[k_code_length_order[i]] =
          br_read_bits(&dec->br, 3);
    }
    if (!read_huffman_code_lengths(
            dec, code_length_code_lengths, alphabet_size, dec->code_lengths)) {
      return 0;
    }
  }

  if (dec->br.eos) {
    set_err(dec, GIMG_ERR_CORRUPT);
    return 0;
  }

  out_tree->nodes = dec->node_pool + dec->node_pool_used;
  out_tree->capacity = dec->node_pool_cap - dec->node_pool_used;
  if (out_tree->capacity < 2) {
    set_err(dec, GIMG_ERR_OOM);
    return 0;
  }
  if (!huff_build(out_tree, dec->code_lengths, alphabet_size)) {
    set_err(dec, GIMG_ERR_CORRUPT);
    return 0;
  }
  dec->node_pool_used += out_tree->used;
  return 1;
}

static int read_huffman_codes(vp8l_dec_t * dec, int xsize, int ysize,
    int color_cache_bits, int allow_recursion) {
  uint32_t * huffman_image = NULL;
  int num_htree_groups = 1;
  int num_htree_groups_max = 1;
  int * mapping = NULL;
  int ok = 0;

  if (allow_recursion && br_read_bits(&dec->br, 1)) {
    const int huffman_precision = 2 + br_read_bits(&dec->br, 3);
    const int huffman_xsize =
        (int)vp8l_subsample_size((uint32_t)xsize, (uint32_t)huffman_precision);
    const int huffman_ysize =
        (int)vp8l_subsample_size((uint32_t)ysize, (uint32_t)huffman_precision);
    const int huffman_pixs = huffman_xsize * huffman_ysize;
    if (!decode_image_stream(
            dec, huffman_xsize, huffman_ysize, 0, &huffman_image)) {
      goto Error;
    }
    dec->huffman_subsample_bits = huffman_precision;
    for (int i = 0; i < huffman_pixs; ++i) {
      const int group = (int)((huffman_image[i] >> 8) & 0xffffu);
      huffman_image[i] = (uint32_t)group;
      if (group >= num_htree_groups_max) {
        num_htree_groups_max = group + 1;
      }
    }
    if (num_htree_groups_max > 1000 ||
        num_htree_groups_max > xsize * ysize) {
      mapping = (int *)gimg_malloc(
          dec->alloc, (size_t)num_htree_groups_max * sizeof(*mapping));
      if (!mapping) {
        set_err(dec, GIMG_ERR_OOM);
        goto Error;
      }
      memset(mapping, 0xff, (size_t)num_htree_groups_max * sizeof(*mapping));
      num_htree_groups = 0;
      for (int i = 0; i < huffman_pixs; ++i) {
        int * mapped = &mapping[huffman_image[i]];
        if (*mapped == -1) {
          *mapped = num_htree_groups++;
        }
        huffman_image[i] = (uint32_t)*mapped;
      }
    }
    else {
      num_htree_groups = num_htree_groups_max;
    }
  }

  if (dec->br.eos) {
    goto Error;
  }

  dec->htree_groups = (vp8l_htree_group_t *)gimg_calloc(
      dec->alloc, (size_t)num_htree_groups, sizeof(*dec->htree_groups));
  if (!dec->htree_groups) {
    set_err(dec, GIMG_ERR_OOM);
    goto Error;
  }
  dec->num_htree_groups = num_htree_groups;
  dec->huffman_image = huffman_image;
  huffman_image = NULL;
  dec->huffman_xsize = dec->huffman_subsample_bits
      ? (int)vp8l_subsample_size(
            (uint32_t)xsize, (uint32_t)dec->huffman_subsample_bits)
      : xsize;
  dec->huffman_mask = (dec->huffman_subsample_bits == 0)
      ? ~0
      : (1 << dec->huffman_subsample_bits) - 1;

  for (int i = 0; i < num_htree_groups_max; ++i) {
    if (mapping != NULL && mapping[i] == -1) {
      for (int j = 0; j < VP8L_HUFFMAN_PER_META; ++j) {
        int alphabet_size = k_alphabet_size[j];
        vp8l_huff_t discard;
        if (j == 0 && color_cache_bits > 0) {
          alphabet_size += 1 << color_cache_bits;
        }
        memset(&discard, 0, sizeof(discard));
        if (!read_huffman_code(dec, alphabet_size, &discard)) {
          goto Error;
        }
        /* Nodes consumed into pool but unused — fine. */
      }
    }
    else {
      const int gi = (mapping == NULL) ? i : mapping[i];
      vp8l_htree_group_t * group = &dec->htree_groups[gi];
      for (int j = 0; j < VP8L_HUFFMAN_PER_META; ++j) {
        int alphabet_size = k_alphabet_size[j];
        if (j == 0 && color_cache_bits > 0) {
          alphabet_size += 1 << color_cache_bits;
        }
        if (!read_huffman_code(dec, alphabet_size, &group->trees[j])) {
          goto Error;
        }
      }
    }
  }
  ok = 1;

Error:
  gimg_free(dec->alloc, mapping);
  if (!ok) {
    gimg_free(dec->alloc, huffman_image);
    clear_metadata(dec);
  }
  return ok;
}

static int decode_image_data(vp8l_dec_t * dec, uint32_t * data, int width,
    int height) {
  int row = 0;
  int col = 0;
  uint32_t * src = data;
  uint32_t * last_cached = src;
  uint32_t * const src_end = data + width * height;
  const int len_code_limit = VP8L_NUM_LITERAL + VP8L_NUM_LENGTH;
  const int color_cache_limit = len_code_limit + dec->color_cache_size;
  vp8l_cache_t * color_cache =
      (dec->color_cache_size > 0) ? &dec->color_cache : NULL;
  const int mask = dec->huffman_mask;
  vp8l_htree_group_t * htree_group = get_htree_group(dec, col, row);

  while (src < src_end) {
    int code;
    if ((col & mask) == 0) {
      htree_group = get_htree_group(dec, col, row);
      if (!htree_group) {
        set_err(dec, GIMG_ERR_CORRUPT);
        return 0;
      }
    }
    code = huff_read_symbol(&htree_group->trees[VP8L_GREEN], &dec->br);
    if (code < 0 || dec->br.eos) {
      set_err(dec, GIMG_ERR_CORRUPT);
      return 0;
    }
    if (code < VP8L_NUM_LITERAL) {
      int red = huff_read_symbol(&htree_group->trees[VP8L_RED], &dec->br);
      int blue = huff_read_symbol(&htree_group->trees[VP8L_BLUE], &dec->br);
      int alpha = huff_read_symbol(&htree_group->trees[VP8L_ALPHA], &dec->br);
      if (red < 0 || blue < 0 || alpha < 0 || dec->br.eos) {
        set_err(dec, GIMG_ERR_CORRUPT);
        return 0;
      }
      *src = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) |
          ((uint32_t)code << 8) | (uint32_t)blue;
      ++src;
      ++col;
      if (col >= width) {
        col = 0;
        ++row;
        if (color_cache) {
          while (last_cached < src) {
            cache_insert(color_cache, *last_cached++);
          }
        }
      }
    }
    else if (code < len_code_limit) {
      const int length_sym = code - VP8L_NUM_LITERAL;
      const int length = get_copy_distance(length_sym, &dec->br);
      const int dist_symbol =
          huff_read_symbol(&htree_group->trees[VP8L_DIST], &dec->br);
      int dist_code;
      int dist;
      if (dist_symbol < 0 || dec->br.eos) {
        set_err(dec, GIMG_ERR_CORRUPT);
        return 0;
      }
      dist_code = get_copy_distance(dist_symbol, &dec->br);
      dist = plane_code_to_distance(width, dist_code);
      if ((src - data) < dist || (src_end - src) < length) {
        set_err(dec, GIMG_ERR_CORRUPT);
        return 0;
      }
      copy_block(src, dist, length);
      src += length;
      col += length;
      while (col >= width) {
        col -= width;
        ++row;
      }
      if (col & mask) {
        htree_group = get_htree_group(dec, col, row);
      }
      if (color_cache) {
        while (last_cached < src) {
          cache_insert(color_cache, *last_cached++);
        }
      }
    }
    else if (code < color_cache_limit) {
      const int key = code - len_code_limit;
      if (!color_cache) {
        set_err(dec, GIMG_ERR_CORRUPT);
        return 0;
      }
      while (last_cached < src) {
        cache_insert(color_cache, *last_cached++);
      }
      *src = cache_lookup(color_cache, key);
      ++src;
      ++col;
      if (col >= width) {
        col = 0;
        ++row;
        while (last_cached < src) {
          cache_insert(color_cache, *last_cached++);
        }
      }
    }
    else {
      set_err(dec, GIMG_ERR_CORRUPT);
      return 0;
    }
  }

  if (color_cache) {
    while (last_cached < src) {
      cache_insert(color_cache, *last_cached++);
    }
  }
  return 1;
}

static int expand_color_map(vp8l_dec_t * dec, int num_colors,
    gimg_webp_vp8l_xform_t * transform) {
  const int final_num_colors = 1 << (8 >> transform->bits);
  uint32_t * new_color_map = (uint32_t *)gimg_malloc(
      dec->alloc, (size_t)final_num_colors * sizeof(*new_color_map));
  if (!new_color_map) {
    set_err(dec, GIMG_ERR_OOM);
    return 0;
  }
  {
    uint8_t * const data = (uint8_t *)transform->data;
    uint8_t * const new_data = (uint8_t *)new_color_map;
    int i;
    new_color_map[0] = transform->data[0];
    for (i = 4; i < 4 * num_colors; ++i) {
      new_data[i] = (uint8_t)((data[i] + new_data[i - 4]) & 0xff);
    }
    for (; i < 4 * final_num_colors; ++i) {
      new_data[i] = 0;
    }
  }
  gimg_free(dec->alloc, transform->data);
  transform->data = new_color_map;
  return 1;
}

static int read_transform(vp8l_dec_t * dec, int * xsize, int ysize) {
  gimg_webp_vp8l_xform_t * transform;
  const int type = br_read_bits(&dec->br, 2);
  int ok = 1;

  if (dec->transforms_seen & (1u << type)) {
    set_err(dec, GIMG_ERR_CORRUPT);
    return 0;
  }
  dec->transforms_seen |= (1u << type);
  if (dec->next_transform >= VP8L_NUM_TRANSFORMS) {
    set_err(dec, GIMG_ERR_CORRUPT);
    return 0;
  }
  transform = &dec->transforms[dec->next_transform++];
  transform->type = type;
  transform->xsize = *xsize;
  transform->ysize = ysize;
  transform->data = NULL;
  transform->bits = 0;

  switch (type) {
  case GIMG_WEBP_VP8L_PREDICTOR:
  case GIMG_WEBP_VP8L_CROSS_COLOR:
    transform->bits = 2 + br_read_bits(&dec->br, 3);
    ok = decode_image_stream(dec,
        (int)vp8l_subsample_size(
            (uint32_t)transform->xsize, (uint32_t)transform->bits),
        (int)vp8l_subsample_size(
            (uint32_t)transform->ysize, (uint32_t)transform->bits),
        0, &transform->data);
    break;
  case GIMG_WEBP_VP8L_COLOR_INDEXING: {
    const int num_colors = br_read_bits(&dec->br, 8) + 1;
    const int bits = (num_colors > 16) ? 0 : (num_colors > 4) ? 1
                                             : (num_colors > 2)  ? 2
                                                                : 3;
    *xsize = (int)vp8l_subsample_size((uint32_t)transform->xsize, (uint32_t)bits);
    transform->bits = bits;
    ok = decode_image_stream(dec, num_colors, 1, 0, &transform->data);
    if (ok && !expand_color_map(dec, num_colors, transform)) {
      return 0;
    }
    break;
  }
  case GIMG_WEBP_VP8L_SUBTRACT_GREEN:
    break;
  default:
    set_err(dec, GIMG_ERR_CORRUPT);
    return 0;
  }
  return ok;
}

static int decode_image_stream(vp8l_dec_t * dec, int xsize, int ysize,
    int is_level0, uint32_t ** decoded_data) {
  int ok = 1;
  int transform_xsize = xsize;
  int transform_ysize = ysize;
  uint32_t * data = NULL;
  int color_cache_bits = 0;

  if (is_level0) {
    while (ok && br_read_bits(&dec->br, 1)) {
      ok = read_transform(dec, &transform_xsize, transform_ysize);
    }
  }

  if (ok && br_read_bits(&dec->br, 1)) {
    color_cache_bits = br_read_bits(&dec->br, 4);
    ok = (color_cache_bits >= 1 && color_cache_bits <= VP8L_MAX_CACHE_BITS);
    if (!ok) {
      set_err(dec, GIMG_ERR_CORRUPT);
      goto End;
    }
  }

  ok = ok &&
      read_huffman_codes(
          dec, transform_xsize, transform_ysize, color_cache_bits, is_level0);
  if (!ok) {
    set_err(dec, GIMG_ERR_CORRUPT);
    goto End;
  }

  if (color_cache_bits > 0) {
    dec->color_cache_size = 1 << color_cache_bits;
    dec->color_cache.hash_bits = color_cache_bits;
    dec->color_cache.hash_shift = 32 - color_cache_bits;
    dec->color_cache.size = dec->color_cache_size;
    dec->color_cache.colors = (uint32_t *)gimg_calloc(
        dec->alloc, (size_t)dec->color_cache_size, sizeof(uint32_t));
    if (!dec->color_cache.colors) {
      ok = 0;
      set_err(dec, GIMG_ERR_OOM);
      goto End;
    }
  }
  else {
    dec->color_cache_size = 0;
  }

  dec->width = transform_xsize;
  dec->height = transform_ysize;

  if (is_level0) {
    goto End;
  }

  {
    const size_t total = (size_t)transform_xsize * (size_t)transform_ysize;
    data = (uint32_t *)gimg_malloc(dec->alloc, total * sizeof(*data));
    if (!data) {
      ok = 0;
      set_err(dec, GIMG_ERR_OOM);
      goto End;
    }
  }

  ok = decode_image_data(dec, data, transform_xsize, transform_ysize);

End:
  if (!ok) {
    gimg_free(dec->alloc, data);
    clear_metadata(dec);
  }
  else {
    if (decoded_data) {
      *decoded_data = data;
    }
    else {
      /* Level-0 leaves Huffman state in place for the subsequent pixel pass. */
    }
    if (!is_level0) {
      clear_metadata(dec);
    }
  }
  return ok;
}

static GIMG_Result apply_all_inverse(vp8l_dec_t * dec, uint32_t * pixels,
    uint32_t ** out_argb) {
  /* Coding buffer may be narrower than the canvas (colour indexing). Apply
   * transforms in reverse into a full canvas_width x canvas_height buffer. */
  const int cw = dec->canvas_width;
  const int ch = dec->canvas_height;
  uint32_t * buf_a = NULL;
  uint32_t * buf_b = NULL;
  uint32_t * in;
  uint32_t * out;
  size_t full_n = (size_t)cw * (size_t)ch;
  size_t coded_n = (size_t)dec->width * (size_t)dec->height;

  buf_a = (uint32_t *)gimg_malloc(dec->alloc, full_n * sizeof(uint32_t));
  buf_b = (uint32_t *)gimg_malloc(dec->alloc, full_n * sizeof(uint32_t));
  if (!buf_a || !buf_b) {
    gimg_free(dec->alloc, buf_a);
    gimg_free(dec->alloc, buf_b);
    return GIMG_ERR_OOM;
  }

  /* Start with coded pixels in buf_a (may be smaller; packed at start). */
  memcpy(buf_a, pixels, coded_n * sizeof(uint32_t));
  in = buf_a;
  out = buf_b;

  if (dec->next_transform == 0) {
    /* No transforms: coded size equals canvas. */
    if (coded_n != full_n) {
      gimg_free(dec->alloc, buf_a);
      gimg_free(dec->alloc, buf_b);
      return GIMG_ERR_CORRUPT;
    }
    *out_argb = buf_a;
    gimg_free(dec->alloc, buf_b);
    return GIMG_OK;
  }

  for (int n = dec->next_transform - 1; n >= 0; --n) {
    const gimg_webp_vp8l_xform_t * xf = &dec->transforms[n];
    gimg_webp_vp8l_inverse_xform(xf, 0, ch, in, out);
    {
      uint32_t * tmp = in;
      in = out;
      out = tmp;
    }
  }

  *out_argb = in;
  if (in == buf_a) {
    gimg_free(dec->alloc, buf_b);
  }
  else {
    gimg_free(dec->alloc, buf_a);
  }
  return GIMG_OK;
}

GIMG_Result gimg_webp_vp8l_decode(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, GIMG_Raster ** out_raster) {
  vp8l_dec_t dec;
  uint32_t * pixels = NULL;
  uint32_t * argb = NULL;
  GIMG_Raster * raster = NULL;
  GIMG_Result r = GIMG_OK;
  int width = 0;
  int height = 0;
  int has_alpha = 0;

  if (!data || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  alloc = gimg_alloc_or_default(alloc);

  memset(&dec, 0, sizeof(dec));
  dec.alloc = alloc;
  dec.err = GIMG_OK;
  br_init(&dec.br, data, size);

  dec.node_pool_cap = VP8L_HUFF_NODE_POOL;
  dec.node_pool = (vp8l_huff_node_t *)gimg_malloc(
      alloc, (size_t)dec.node_pool_cap * sizeof(*dec.node_pool));
  if (!dec.node_pool) {
    return GIMG_ERR_OOM;
  }

  if (br_read_bits(&dec.br, 8) != VP8L_MAGIC) {
    r = GIMG_ERR_CORRUPT;
    goto Done;
  }
  width = br_read_bits(&dec.br, 14) + 1;
  height = br_read_bits(&dec.br, 14) + 1;
  has_alpha = br_read_bits(&dec.br, 1);
  (void)has_alpha;
  if (br_read_bits(&dec.br, 3) != 0 || dec.br.eos) {
    r = GIMG_ERR_CORRUPT;
    goto Done;
  }
  dec.canvas_width = width;
  dec.canvas_height = height;

  if (!decode_image_stream(&dec, width, height, 1, NULL)) {
    r = (dec.err != GIMG_OK) ? dec.err : GIMG_ERR_CORRUPT;
    goto Done;
  }

  {
    const size_t n = (size_t)dec.width * (size_t)dec.height;
    pixels = (uint32_t *)gimg_malloc(alloc, n * sizeof(*pixels));
    if (!pixels) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
  }

  if (!decode_image_data(&dec, pixels, dec.width, dec.height)) {
    r = (dec.err != GIMG_OK) ? dec.err : GIMG_ERR_CORRUPT;
    goto Done;
  }

  r = apply_all_inverse(&dec, pixels, &argb);
  if (r != GIMG_OK) {
    goto Done;
  }
  gimg_free(alloc, pixels);
  pixels = NULL;

  r = gimg_raster_create_with_allocator(alloc, (uint32_t)dec.canvas_width,
      (uint32_t)dec.canvas_height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL,
      0, &raster);
  if (r != GIMG_OK) {
    goto Done;
  }

  {
    uint8_t * dst = (uint8_t *)gimg_raster_pixels(raster);
    const size_t stride = gimg_raster_stride_bytes(raster);
    for (int y = 0; y < dec.canvas_height; ++y) {
      uint8_t * row = dst + (size_t)y * stride;
      const uint32_t * src = argb + (size_t)y * (size_t)dec.canvas_width;
      for (int x = 0; x < dec.canvas_width; ++x) {
        const uint32_t p = src[x];
        row[4 * x + 0] = (uint8_t)((p >> 16) & 0xffu);
        row[4 * x + 1] = (uint8_t)((p >> 8) & 0xffu);
        row[4 * x + 2] = (uint8_t)(p & 0xffu);
        row[4 * x + 3] = (uint8_t)((p >> 24) & 0xffu);
      }
    }
  }

  *out_raster = raster;
  raster = NULL;
  r = GIMG_OK;

Done:
  if (raster) {
    gimg_raster_destroy(raster);
  }
  gimg_free(alloc, pixels);
  gimg_free(alloc, argb);
  clear_metadata(&dec);
  clear_transforms(&dec);
  gimg_free(alloc, dec.node_pool);
  return r;
}

GIMG_Result gimg_webp_vp8l_decode_alpha(const unsigned char * data, size_t size,
    uint32_t width, uint32_t height, const GIMG_Allocator * alloc,
    uint8_t ** out_alpha) {
  vp8l_dec_t dec;
  uint32_t * pixels = NULL;
  uint32_t * argb = NULL;
  uint8_t * alpha = NULL;
  GIMG_Result r = GIMG_OK;

  if (!data || !out_alpha || width == 0u || height == 0u) {
    return GIMG_ERR_INTERNAL;
  }
  *out_alpha = NULL;
  alloc = gimg_alloc_or_default(alloc);

  memset(&dec, 0, sizeof(dec));
  dec.alloc = alloc;
  dec.err = GIMG_OK;
  br_init(&dec.br, data, size);
  dec.canvas_width = (int)width;
  dec.canvas_height = (int)height;

  dec.node_pool_cap = VP8L_HUFF_NODE_POOL;
  dec.node_pool = (vp8l_huff_node_t *)gimg_malloc(
      alloc, (size_t)dec.node_pool_cap * sizeof(*dec.node_pool));
  if (!dec.node_pool) {
    return GIMG_ERR_OOM;
  }

  /* ALPH method-1 streams omit the VP8L frame header and start at transforms. */
  if (!decode_image_stream(&dec, (int)width, (int)height, 1, NULL)) {
    r = (dec.err != GIMG_OK) ? dec.err : GIMG_ERR_CORRUPT;
    goto Done;
  }

  {
    const size_t n = (size_t)dec.width * (size_t)dec.height;
    pixels = (uint32_t *)gimg_malloc(alloc, n * sizeof(*pixels));
    if (!pixels) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
  }

  if (!decode_image_data(&dec, pixels, dec.width, dec.height)) {
    r = (dec.err != GIMG_OK) ? dec.err : GIMG_ERR_CORRUPT;
    goto Done;
  }

  r = apply_all_inverse(&dec, pixels, &argb);
  if (r != GIMG_OK) {
    goto Done;
  }
  gimg_free(alloc, pixels);
  pixels = NULL;

  {
    const size_t n = (size_t)width * (size_t)height;
    alpha = (uint8_t *)gimg_malloc(alloc, n);
    if (!alpha) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    /* Alpha samples ride in the green channel (libwebp ExtractAlphaRows). */
    for (size_t i = 0; i < n; ++i) {
      alpha[i] = (uint8_t)((argb[i] >> 8) & 0xffu);
    }
  }

  *out_alpha = alpha;
  alpha = NULL;
  r = GIMG_OK;

Done:
  gimg_free(alloc, alpha);
  gimg_free(alloc, pixels);
  gimg_free(alloc, argb);
  clear_metadata(&dec);
  clear_transforms(&dec);
  gimg_free(alloc, dec.node_pool);
  return r;
}
