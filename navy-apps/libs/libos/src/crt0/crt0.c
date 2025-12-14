#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

int main(int argc, char *argv[], char *envp[]);
void __libc_init_array (void);
extern char **environ;
void call_main(uintptr_t *args) 
{
  // Fallback empty vectors if args is not provided
  static char *empty[] = { NULL };

  int argc = 0;
  char **argv = empty;
  char **envp = empty;

  if (args != NULL) 
  {
    // args points to the word "argc" on the initial user stack
    argc = (int)args[0];

    // argv starts right after argc
    argv = (char **)&args[1];

    // envp starts after argv[argc] (which is the NULL terminator)
    envp = argv + argc + 1;

    // If the OS does not provide envp, ensure it is a valid empty list
    if (envp == NULL) 
    {
      envp = empty;
    }
  }

  // Export envp for getenv() and related libc functions
  environ = envp;

  // Init C++ global constructors, and other runtime init hooks
  __libc_init_array();

  // Transfer control to user main()
  exit(main(argc, argv, envp));

  assert(0);
}