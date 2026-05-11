#include <stdio.h>
#include <NDL.h>

int main()
{
    NDL_Init(0);
    /*
   * This is an interactive smoke test for the /dev/events to NDL boundary.
   * It prints raw event strings so keyboard and window-manager protocol
   * changes are visible without pulling in SDL's translation layer.
   */
    while (1)
    {
        char buf[64];
        size_t n = NDL_PollEvent(buf, sizeof(buf));

        if (n)
        {
            printf("receive event: %.*s\n", (int)n, (char *)buf);
        }
    }
    return 0;
}
