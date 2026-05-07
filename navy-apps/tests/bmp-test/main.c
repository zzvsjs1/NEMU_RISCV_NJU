#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <NDL.h>
#include <BMP.h>

int main() {
  NDL_Init(0);
  int w, h;
  /*
   * BMP_Load reads from the ramdisk and NDL_DrawRect pushes the decoded pixels
   * straight to the canvas. Keeping this path SDL-free makes it a narrow test
   * of the image loader plus NDL framebuffer contract.
   */
  void *bmp = BMP_Load("/share/pictures/projectn.bmp", &w, &h);
  assert(bmp);
  NDL_OpenCanvas(&w, &h);
  NDL_DrawRect(bmp, 0, 0, w, h);
  free(bmp);
  NDL_Quit();
  printf("Test ends! Spinning...\n");
  while (1);
  return 0;
}
