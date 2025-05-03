// #include <stdio.h>
// #include <stdbool.h>
// #include <stdlib.h>
// #include <stdint.h>
// #include <inttypes.h>
// #include <sys/time.h>
// #include <sys/types.h>
// #include <unistd.h>
// #include <NDL.h>

// /**
//  * print_periodically_busy_int
//  * ---------------------------
//  * Prints `msg` every `interval_us` microseconds, by spinning in a tight loop,
//  * using only integer arithmetic (no floating point).
//  *
//  * msg         : the string to print (null-terminated)
//  * interval_us : time between prints, in microseconds (e.g. 500000 for 0.5 s)
//  */
// void print_periodically_busy_int(const char *msg, long interval_us) 
// {
//     struct timeval now, next;

//     // 1) get starting time
//     if (gettimeofday(&now, NULL) != 0) 
//     {
//         perror("gettimeofday");
//         exit(EXIT_FAILURE);
//     }

//     // initialize next = now
//     next = now;

//     uint32_t secs = 0;
//     bool half = true;

//     // 2) infinite loop: wait until next, then print, then schedule next
//     while (1) {
//         // busy-wait until now >= next
//         do {
//             if (gettimeofday(&now, NULL) != 0) 
//             {
//                 perror("gettimeofday");
//                 exit(EXIT_FAILURE);
//             }

//             // compare seconds first, then microseconds
//         } while (now.tv_sec < next.tv_sec
//               || (now.tv_sec == next.tv_sec && now.tv_usec < next.tv_usec));

//         secs += half ? 0 : 1;

//         // it’s time (or just past time) to print
//         printf("%s %" PRIu32 ".%s" "\n", msg, secs, half ? "5" : "0");

//         half = !half;

//         // schedule the next wake-up by adding interval_us
//         next.tv_usec += interval_us;
//         // carry overflow of microseconds into seconds
//         next.tv_sec  += next.tv_usec / 1000000;
//         next.tv_usec  = next.tv_usec % 1000000;
//     }
// }

// int main(void) 
// {
//     // prints "Tick" every 500 000 µs (0.5 s), busy-waiting in between
//     print_periodically_busy_int("Tick", 500000L);
//     return 0;  // never reached
// }

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <NDL.h>

/**
 * print_periodically_busy_int
 * ---------------------------
 * Prints `msg` every `interval_us` microseconds, by spinning in a tight loop,
 * using only integer arithmetic (no floating point), powered by NDL_GetTicks().
 *
 * msg         : the string to print (null-terminated)
 * interval_us : time between prints, in microseconds (e.g. 500000 for 0.5 s)
 */
void print_periodically_busy_int(const char *msg, long interval_us) 
{
    // 1) convert microsecond interval to whole milliseconds
    uint32_t interval_ms = interval_us / 1000;  
    if (interval_ms == 0) interval_ms = 1;  // at least 1 ms

    // 2) record the starting tick (ms since NDL_Init)
    uint32_t next_tick = NDL_GetTicks();

    uint32_t secs = 0;
    bool half = true;

    // 3) infinite loop: wait until NDL_GetTicks() >= next_tick, then print, then bump next_tick
    for (;;) {
        // busy-wait for the next tick
        while (NDL_GetTicks() < next_tick) 
        {
            // nothing
        }

        // advance our “seconds” counter every two prints
        if (!half) secs++;

        // print with “.5” for half==true, “.0” for half==false
        printf("%s %" PRIu32 ".%s\n", msg, secs, half ? "5" : "0");

        // flip half-second flag
        half = !half;

        // schedule the next wake-up
        next_tick += interval_ms;
    }
}

int main(void) 
{
    // Example: prints "Tick" every 500000 µs (0.5 s)
    print_periodically_busy_int("Tick", 500000L);
    return 0;  // never reached
}

