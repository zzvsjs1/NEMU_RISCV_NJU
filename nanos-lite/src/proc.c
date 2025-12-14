#include <proc.h>

#define MAX_NR_PROC 4

void context_uload(PCB *pcb, const char *filename, char *const argv[], char *const envp[]);

static PCB pcb[MAX_NR_PROC] __attribute__((used)) = {};
static PCB pcb_boot = {};
PCB *current = NULL;

void switch_boot_pcb() { current = &pcb_boot; }

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
  context_kload(&pcb[0], hello_fun, (void *)"A");

  Log("Initializing processes...");

  static char *const argv_pal[] = { "/bin/menu", NULL, NULL };
  static char *const envp_empty[] = { NULL };
  context_uload(&pcb[1], "/bin/menu", argv_pal, envp_empty);
  
  // Initialize current to the boot PCB,
  // so the first schedule() call switches to pcb[0].
  switch_boot_pcb();

  //void naive_uload(PCB *pcb, const char *filename);
  //naive_uload(NULL, "/bin/pal");
}

Context *schedule(Context *prev) 
{ 
  // Do RR schedule.
  
  // Save the context of the currently running PCB.
  if (current != NULL) 
  {
    current->cp = prev;
  }

  // Pick the next runnable PCB.
  // First switch: from boot PCB to pcb[0].
  if (current == &pcb_boot) 
  {
    current = &pcb[0];
  } 
  else 
  {
    current = (current == &pcb[0]) ? &pcb[1] : &pcb[0];
  }

  // Return the context pointer of the next PCB.
  return current->cp;
}
