#include <stdio.h>
#include <NDL.h>

int main() {
  NDL_Init(0);
  while (1) {
    char buf[64];
    size_t n = NDL_PollEvent(buf, sizeof(buf));
    if (n) {
      printf("receive event: %.*s\n", (int)n, (char*)buf);
    }
  }
  return 0;
}
