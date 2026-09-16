/*
 * Generator for the four-component JPEG fixtures.
 *
 * Writes files libjpeg-turbo itself produced, so that the decode tests have an
 * oracle whose encoder is not this library:
 *
 *   ycck_ljt_420.jpg, ycck_ljt_422.jpg  YCCK (Adobe APP14 transform 2) with the
 *                                       chrominance components subsampled, the
 *                                       case that needs the triangle filter of
 *                                       jdsample.c and had no fixture before.
 *   cmyk_ljt_sub.jpg                    CMYK (transform 0) with sampling
 *                                       factors set by hand, so that an
 *                                       upsampled four-component frame that is
 *                                       *not* YCCK is covered too.
 *
 * The pattern is the one tests/codec/jpeg/test_jpeg_encode.cpp builds for the
 * encode fixtures, so the same picture appears on both sides.
 *
 * Build against a libjpeg-turbo checkout:
 *   cc -o mk_cmyk mk_cmyk.c -I<ljt-src> -I<ljt-build> <ljt-build>/libjpeg.a
 * then run cmyk_ref.c over each output to make the matching .raw.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define W 33u
#define H 17u

static void fill(unsigned char * buf) {
  for (unsigned y = 0; y < H; y++) {
    for (unsigned x = 0; x < W; x++) {
      unsigned char * p = buf + ((size_t)y * W + x) * 4;
      p[0] = (unsigned char)((x * 7 + y * 3) & 0xFF);
      p[1] = (unsigned char)((x * 3 + y * 11) & 0xFF);
      p[2] = (unsigned char)((x * 13 + y * 5) & 0xFF);
      p[3] = (unsigned char)((x + y * 2) & 0xFF);
    }
  }
}

static int write_one(const char * path, int ycck, int h0, int v0) {
  unsigned char * buf = (unsigned char *)malloc((size_t)W * H * 4);
  if (!buf) return 1;
  fill(buf);
  FILE * f = fopen(path, "wb");
  if (!f) { free(buf); return 1; }
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr e;
  c.err = jpeg_std_error(&e);
  jpeg_create_compress(&c);
  jpeg_stdio_dest(&c, f);
  c.image_width = W;
  c.image_height = H;
  c.input_components = 4;
  c.in_color_space = JCS_CMYK;
  jpeg_set_defaults(&c);
  /* JCS_YCCK makes libjpeg apply its own CMYK -> YCCK transform and write the
   * Adobe marker with transform 2; JCS_CMYK writes transform 0 and passes the
   * samples through. */
  jpeg_set_colorspace(&c, ycck ? JCS_YCCK : JCS_CMYK);
  c.comp_info[0].h_samp_factor = h0;
  c.comp_info[0].v_samp_factor = v0;
  c.comp_info[1].h_samp_factor = 1;
  c.comp_info[1].v_samp_factor = 1;
  c.comp_info[2].h_samp_factor = 1;
  c.comp_info[2].v_samp_factor = 1;
  c.comp_info[3].h_samp_factor = h0;
  c.comp_info[3].v_samp_factor = v0;
  jpeg_set_quality(&c, 90, TRUE);
  jpeg_start_compress(&c, TRUE);
  while (c.next_scanline < H) {
    JSAMPROW row = buf + (size_t)c.next_scanline * W * 4;
    jpeg_write_scanlines(&c, &row, 1);
  }
  jpeg_finish_compress(&c);
  jpeg_destroy_compress(&c);
  fclose(f);
  free(buf);
  printf("wrote %s\n", path);
  return 0;
}

int main(void) {
  int r = 0;
  r |= write_one("ycck_ljt_420.jpg", 1, 2, 2);
  r |= write_one("ycck_ljt_422.jpg", 1, 2, 1);
  r |= write_one("cmyk_ljt_sub.jpg", 0, 2, 2);
  return r;
}
