/* Decode a JPEG with libjpeg at its native precision, with block smoothing
 * under our control (djpeg's -nosmooth is the fancy-upsampling switch, not this).
 * Usage: ljdec [-nobsmooth] in.jpg out.raw
 * Output: u32 w, u32 h, u32 nch, u32 precision, then samples as u16 LE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
int main(int argc, char **argv) {
  int bsmooth = 1, ai = 1;
  if (argc > 1 && !strcmp(argv[1], "-nobsmooth")) { bsmooth = 0; ai = 2; }
  FILE *f = fopen(argv[ai], "rb");
  if (!f) return 1;
  struct jpeg_decompress_struct c; struct jpeg_error_mgr e;
  c.err = jpeg_std_error(&e);
  jpeg_create_decompress(&c);
  jpeg_stdio_src(&c, f);
  jpeg_read_header(&c, TRUE);
  c.do_block_smoothing = bsmooth ? TRUE : FALSE;
  c.do_fancy_upsampling = TRUE;
  jpeg_start_decompress(&c);
  FILE *o = fopen(argv[ai+1], "wb");
  unsigned w = c.output_width, h = c.output_height, n = c.output_components;
  unsigned prec = (unsigned)c.data_precision;
  fwrite(&w,4,1,o); fwrite(&h,4,1,o); fwrite(&n,4,1,o); fwrite(&prec,4,1,o);
  size_t rb = (size_t)w * n;
  unsigned short *out = (unsigned short *)malloc(rb * sizeof(unsigned short));
  if (prec <= 8) {
    JSAMPROW row = (JSAMPROW)malloc(rb);
    JSAMPARRAY arr = &row;
    while (c.output_scanline < h) {
      jpeg_read_scanlines(&c, arr, 1);
      for (size_t i = 0; i < rb; i++) out[i] = row[i];
      fwrite(out, 2, rb, o);
    }
    free(row);
  } else if (prec <= 12) {
    J12SAMPROW row = (J12SAMPROW)malloc(rb * sizeof(short));
    J12SAMPARRAY arr = &row;
    while (c.output_scanline < h) {
      jpeg12_read_scanlines(&c, arr, 1);
      for (size_t i = 0; i < rb; i++) out[i] = (unsigned short)row[i];
      fwrite(out, 2, rb, o);
    }
    free(row);
  } else {
    J16SAMPROW row = (J16SAMPROW)malloc(rb * sizeof(unsigned short));
    J16SAMPARRAY arr = &row;
    while (c.output_scanline < h) {
      jpeg16_read_scanlines(&c, arr, 1);
      for (size_t i = 0; i < rb; i++) out[i] = row[i];
      fwrite(out, 2, rb, o);
    }
    free(row);
  }
  free(out);
  jpeg_finish_decompress(&c); jpeg_destroy_decompress(&c);
  fclose(o); fclose(f);
  return 0;
}
