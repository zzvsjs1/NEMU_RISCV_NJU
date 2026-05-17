#include <proc.h>

enum
{
    FOREGROUND_QUANTA = 5,
};

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[]);
void device_capture_foreground_before_switch(void);
void device_note_foreground_switch(void);
void device_restore_foreground_on_schedule(void);

static PCB pcb[MAX_NR_PROC] __attribute__((used)) = {};
static PCB pcb_boot = {};
PCB *current = NULL;
static PCB *fg_pcb = &pcb[0];
/* The foreground app normally runs continuously.  If the optional background
 * slot is loaded, this small budget lets it make progress without giving it
 * ownership of foreground-only devices such as /dev/fb and /dev/sb.
 */
static int foreground_budget = FOREGROUND_QUANTA;

void switch_boot_pcb() { current = &pcb_boot; }

static bool pcb_runnable(PCB *pcb)
{
    return pcb != NULL && pcb->cp != NULL;
}

// Translate PCB pointers to process-table slots without exposing pcb[] itself.
static int pcb_index_of(PCB *target)
{
    for (int i = 0; i < MAX_NR_PROC; i++)
    {
        if (target == &pcb[i])
        {
            return i;
        }
    }

    return -1;
}

int current_pcb_index(void)
{
    return pcb_index_of(current);
}

int foreground_pcb_index(void)
{
    return pcb_index_of(fg_pcb);
}

bool switch_fg_pcb(int index)
{
    /*
     * Foreground switching is a policy decision, not a new address-space
     * primitive. The selected PCB will become active on the next schedule()
     * return, and the trap return path will then load that PCB's saved satp.
     */
    assert(index >= 0 && index < NR_FOREGROUND_PROC);

    PCB *next = &pcb[index];
    assert(pcb_runnable(next));

    if (fg_pcb != next)
    {
        /*
         * Snapshot the old foreground display before changing fg_pcb.  The device
         * layer identifies the old owner through foreground_pcb_index(), so this
         * ordering is what lets lazy framebuffer backing preserve the outgoing
         * app's last physical frame.
         *
         * Reset the budget and defer audio restoration until the selected process
         * is actually scheduled.  The old foreground process may still be inside a
         * syscall while handling the hotkey.
         */
        device_capture_foreground_before_switch();
        fg_pcb = next;
        foreground_budget = FOREGROUND_QUANTA;
        device_note_foreground_switch();
        Log("Switch foreground to pcb[%d]", index);
        return true;
    }

    return false;
}

void context_kload(PCB *pcb, void (*entry)(void *), void *arg)
{
    // Build an Area that describes this PCB's kernel stack range.
    // Using (pcb + 1) as the end is consistent with yield-os,
    // because the union size is STACK_SIZE (aligned), so pcb + 1 points to stack end.
    Area kstack = (Area){
        .start = pcb->stack,
        .end = pcb + 1,
    };

    // Create an initial context on the kernel stack,
    // and record the returned context pointer into PCB.
    pcb->cp = kcontext(kstack, entry, arg);
}

// Must never return, otherwise, it will return 0 as the context.
void hello_fun(void *arg)
{
    // Interpret arg as a C string, because pointer may have 32/64 bit issue in some cases.
    // const char *name = (const char *)arg;
    // int j = 1;
    while (1)
    {
        // Log("Hello World from Nanos-lite, arg '%s', iteration %d!", name, j);
        // j++;
        yield();
    }
}

void init_proc()
{
    Log("Initializing processes...");

    static char *const envp_empty[] = {NULL};

#if defined(__ISA_RISCV64__)
    /*
     * Keep the first RV64 Nanos/Navy target small and deterministic.  PAL,
     * FCEUX, and ONScripter exercise large graphics/audio stacks and still need
     * separate RV64 compatibility work; /bin/hello proves the syscall, loader,
     * Sv39, context-switch, and user-mode return path first.
     */
    static char *const argv_hello[] = {"/bin/hello", NULL};
    context_uload(&pcb[0], "/bin/hello", argv_hello, envp_empty);
#else
    static char *const argv_pal[] = {"/bin/pal", NULL};
    static char *const argv_fceux[] = {"/bin/fceux", "/share/games/nes/c.nes", NULL};
    static char *const argv_onscripter[] = {"/bin/onscripter", "-r", "/share/games/ons", NULL};
    // static char *const argv_nslider[] = { "/bin/nslider", NULL };
    // static char *const argv_hello[] = { "/bin/hello", NULL };
    context_uload(&pcb[0], "/bin/pal", argv_pal, envp_empty);
    context_uload(&pcb[1], "/bin/fceux", argv_fceux, envp_empty);
    context_uload(&pcb[2], "/bin/onscripter", argv_onscripter, envp_empty);
    // context_uload(&pcb[2], "/bin/nslider", argv_nslider, envp_empty);
    // context_uload(&pcb[3], "/bin/hello", argv_hello, envp_empty);
#endif
    fg_pcb = &pcb[0];
    foreground_budget = FOREGROUND_QUANTA;

    // Initialize current to the boot PCB,
    // so the first schedule() call switches to pcb[0].
    switch_boot_pcb();

    // void naive_uload(PCB *pcb, const char *filename);
    // naive_uload(NULL, "/bin/pal");
}

Context *schedule(Context *prev)
{
    // Save the context of the currently running PCB.
    if (current != NULL)
    {
        current->cp = prev;
    }

    // Pick the next runnable PCB.
    // First switch: from boot PCB to the selected foreground process.
    if (current == &pcb_boot)
    {
        // First entry into user space: keep boot as a fake previous owner so the
        // scheduler has somewhere to save the boot trap context.
        current = fg_pcb;
    }
    else if (current == fg_pcb)
    {
        /* Only the optional background slot is time-shared here.  Other foreground
         * apps remain dormant until selected, so their framebuffer/audio state can
         * be restored as a simple foreground switch instead of a true compositor.
         */
        if (pcb_runnable(&pcb[HELLO_PROC]) && foreground_budget-- <= 0)
        {
            foreground_budget = FOREGROUND_QUANTA;
            current = &pcb[HELLO_PROC];
        }
    }
    else
    {
        current = fg_pcb;
    }

    assert(pcb_runnable(current));

    if (current == fg_pcb)
    {
        // Shared foreground devices are restored only when the selected foreground
        // PCB is about to run. This avoids restoring audio/video while another PCB
        // is still finishing the syscall that requested the switch.
        // It also prevents the old app's final callback/poll from using the new
        // app's restored device format after an F1/F2/F3 hand-off.
        device_restore_foreground_on_schedule();
    }

    // Return the context pointer of the next PCB.
    return current->cp;
}
