#include <common.h>
#include <device/alarm.h>
#include <sys/time.h>
#include <signal.h>

#define MAX_HANDLER 8

static alarm_handler_t handler[MAX_HANDLER] = {};
static int idx = 0;

void add_alarm_handle(alarm_handler_t h)
{
    /*
   * Alarm callbacks run from the virtual-timer signal path, so registration is
   * intentionally simple and completed during device initialisation.  The fixed
   * table keeps the signal handler from needing allocation or list traversal
   * metadata that could be unsafe while the host process is interrupted.
   */
    assert(idx < MAX_HANDLER);
    handler[idx++] = h;
}

static void alarm_sig_handler(int signum)
{
    /*
   * ITIMER_VIRTUAL advances only while NEMU is executing on the host CPU.  This
   * makes periodic device interrupts depend on simulated execution time rather
   * than wall-clock sleep, which is the contract timer.c relies on.
   */
    int i;
    for (i = 0; i < idx; i++)
    {
        handler[i]();
    }
}

void init_alarm()
{
    struct sigaction s;
    memset(&s, 0, sizeof(s));
    s.sa_handler = alarm_sig_handler;
    int ret = sigaction(SIGVTALRM, &s, NULL);
    Assert(ret == 0, "Can not set signal handler");

    struct itimerval it = {};
    it.it_value.tv_sec = 0;
    it.it_value.tv_usec = 1000000 / TIMER_HZ;
    it.it_interval = it.it_value;
    ret = setitimer(ITIMER_VIRTUAL, &it, NULL);
    Assert(ret == 0, "Can not set timer");
}
