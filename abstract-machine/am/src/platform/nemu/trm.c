#include <am.h>
#include <nemu.h>

extern char _heap_start;
int main(const char *args);

Area heap = RANGE(&_heap_start, PMEM_END);
static const char mainargs[MAINARGS_MAX_LEN] = MAINARGS_PLACEHOLDER; // defined in CFLAGS

void putch(char ch)
{
    // The TRM console is just NEMU's serial port. There is no buffering here, so
    // every character becomes an immediate MMIO write visible to the emulator.
    outb(SERIAL_PORT, ch);
}

void halt(int code)
{
    // nemu_trap() is the agreed exit path between AM guests and the emulator.
    // Returning from it would mean the emulator did not consume the halt request,
    // so spin rather than falling through into unmapped code.
    nemu_trap(code);

    // should not reach here
    while (1)
        ;
}

void _trm_init()
{
    // Bare-metal startup has no argc/argv setup from an operating system. The
    // build system patches MAINARGS_PLACEHOLDER into mainargs and AM passes that
    // single string directly to the kernel or test entry point.
    int ret = main(mainargs);
    halt(ret);
}
