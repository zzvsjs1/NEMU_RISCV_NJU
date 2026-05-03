#include <proc.h>

enum {
  FOREGROUND_QUANTA = 5,
};

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[]);

static PCB pcb[MAX_NR_PROC] __attribute__((used)) = {};
static PCB pcb_boot = {};
PCB *current = NULL;
static PCB *fg_pcb = &pcb[0];
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

void switch_fg_pcb(int index)
{
  assert(index >= 0 && index < NR_FOREGROUND_PROC);

  PCB *next = &pcb[index];
  assert(pcb_runnable(next));

  if (fg_pcb != next)
  {
    fg_pcb = next;
    foreground_budget = FOREGROUND_QUANTA;
    Log("Switch foreground to pcb[%d]", index);
  }
}

void context_kload(PCB* pcb, void (*entry)(void *), void* arg)
{
  // Build an Area that describes this PCB's kernel stack range.
  // Using (pcb + 1) as the end is consistent with yield-os,
  // because the union size is STACK_SIZE (aligned), so pcb + 1 points to stack end.
  Area kstack = (Area) {
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
  //const char *name = (const char *)arg;
  //int j = 1;
  while (1) {
    //Log("Hello World from Nanos-lite, arg '%s', iteration %d!", name, j);
    //j++;
    yield();
  }
}

void init_proc() 
{
  Log("Initializing processes...");

  static char *const argv_pal[] = { "/bin/pal", NULL };
  static char *const argv_bird[] = { "/bin/bird", NULL };
  static char *const argv_nslider[] = { "/bin/nslider", NULL };
  static char *const argv_hello[] = { "/bin/hello", NULL };
  static char *const envp_empty[] = { NULL };
  context_uload(&pcb[0], "/bin/pal", argv_pal, envp_empty);
  context_uload(&pcb[1], "/bin/bird", argv_bird, envp_empty);
  context_uload(&pcb[2], "/bin/nslider", argv_nslider, envp_empty);
  context_uload(&pcb[3], "/bin/hello", argv_hello, envp_empty);
  fg_pcb = &pcb[0];
  foreground_budget = FOREGROUND_QUANTA;
  
  // Initialize current to the boot PCB,
  // so the first schedule() call switches to pcb[0].
  switch_boot_pcb();

  //void naive_uload(PCB *pcb, const char *filename);
  //naive_uload(NULL, "/bin/pal");
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
    current = fg_pcb;
  } 
  else if (current == fg_pcb)
  {
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

  // Return the context pointer of the next PCB.
  return current->cp;
}
