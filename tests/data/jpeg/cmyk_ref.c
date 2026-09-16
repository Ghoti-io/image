/* libjpeg oracle for CMYK/YCCK fixtures: writes the project's .raw format
 * (1 byte mode=2, u32 LE width, u32 LE height, then w*h*4 samples), exactly as
 * dump_jpeg_pixels_ref -o does. */
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>
int main(int argc, char ** argv) {
  if (argc < 3) { fprintf(stderr, "usage: cmyk_ref out.raw in.jpg\n"); return 2; }
  FILE * f = fopen(argv[2], "rb");
  if (!f) return 1;
  struct jpeg_decompress_struct c; struct jpeg_error_mgr e;
  c.err = jpeg_std_error(&e);
  jpeg_create_decompress(&c);
  jpeg_stdio_src(&c, f);
  jpeg_read_header(&c, TRUE);
  jpeg_start_decompress(&c);
  if (c.output_components != 4) {
    fprintf(stderr, "not 4-component (%d)\n", c.output_components); return 3;
  }
  unsigned w = c.output_width, h = c.output_height;
  unsigned char * buf = malloc((size_t)w * h * 4);
  JSAMPROW row;
  while (c.output_scanline < h) {
    row = buf + (size_t)c.output_scanline * w * 4;
    jpeg_read_scanlines(&c, &row, 1);
  }
  jpeg_finish_decompress(&c);
  jpeg_destroy_decompress(&c);
  fclose(f);
  FILE * o = fopen(argv[1], "wb");
  unsigned char hdr[9];
  hdr[0] = 2;
  hdr[1] = (unsigned char)(w & 0xFF); hdr[2] = (unsigned char)((w >> 8) & 0xFF);
  hdr[3] = (unsigned char)((w >> 16) & 0xFF); hdr[4] = (unsigned char)((w >> 24) & 0xFF);
  hdr[5] = (unsigned char)(h & 0xFF); hdr[6] = (unsigned char)((h >> 8) & 0xFF);
  hdr[7] = (unsigned char)((h >> 16) & 0xFF); hdr[8] = (unsigned char)((h >> 24) & 0xFF);
  fwrite(hdr, 1, 9, o);
  fwrite(buf, 1, (size_t)w * h * 4, o);
  fclose(o);
  printf("%s: %ux%u CMYK\n", argv[1], w, h);
  return 0;
}
