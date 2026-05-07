#include <am.h>

Area heap;

void putch(char ch) 
{
    // Route AM console output through libc stdout. On Navy this eventually
    // reaches the process file descriptor rather than a bare-metal UART.
    (void)putchar(ch);
}

void halt(int code) 
{
    // AM apps model termination via halt(); under Navy the closest
    // process-level boundary is libc exit with the requested status code.
    exit(code);
}
