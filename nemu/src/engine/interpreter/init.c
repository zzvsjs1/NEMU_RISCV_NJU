#include <cpu/cpu.h>

void sdb_mainloop();

/*
 * Start the interpreter front end.  AM targets run the loaded program directly;
 * hosted targets enter the simple debugger so the user can issue commands such
 * as `c' and `si' before cpu_exec() is called.
 */
void engine_start()
{
#ifdef CONFIG_TARGET_AM
    cpu_exec(-1);
#else
    /* Receive commands from user. */
    sdb_mainloop();
#endif
}
