#include <unistd.h>
#include <stdio.h>

int main()
{
    /*
   * Keep one direct write() before printf() so the test covers both the raw
   * syscall path and stdio buffering through Navy's process stdout.
   */
    write(1, "Hello World!\n", 13);
    int i = 2;
    volatile int j = 0;
    while (1)
    {
        j++;

        if (j == 80000)
        {
            printf("Hello World from Navy-apps for the %dth time!\n", i++);
            j = 0;
        }
    }
    return 0;
}
