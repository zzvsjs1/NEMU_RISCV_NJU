#include <nterm.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <SDL.h>

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

char handle_key(SDL_Event *ev);

#define SH_MAX_ARGS 32
#define SH_MAX_PATH 128

extern char **environ;

char handle_key(SDL_Event *ev);

static int sh_split_argv(char *s, char *argv[], int max_args) 
{
  int argc = 0;

  while (*s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') break;

    if (argc >= max_args - 1) break;

    char quote = 0;
    if (*s == '"' || *s == '\'') {
      quote = *s;
      s++;
      argv[argc++] = s;
      while (*s && *s != quote) s++;
      if (*s == quote) {
        *s = '\0';
        s++;
      }
    } else {
      argv[argc++] = s;
      while (*s && !isspace((unsigned char)*s)) s++;
      if (*s) {
        *s = '\0';
        s++;
      }
    }
  }

  argv[argc] = NULL;
  return argc;
}

static const char *sh_resolve_path(const char *cmd, char *buf, size_t bufsz) 
{
  if (cmd == NULL || cmd[0] == '\0') return "";

  if (strchr(cmd, '/')) return cmd;

  snprintf(buf, bufsz, "%s", cmd);
  return buf;
}

static void sh_printf(const char *format, ...) {
  static char buf[256] = {};
  va_list ap;
  va_start(ap, format);
  int len = vsnprintf(buf, 256, format, ap);
  va_end(ap);
  term->write(buf, len);
}

static void sh_banner() {
  sh_printf("Built-in Shell in NTerm (NJU Terminal)\n\n");
}

static void sh_prompt() {
  sh_printf("sh> ");
}

static void sh_handle_cmd(const char *cmd) {
  // 1) Make a mutable copy and strip trailing newline
  char *line = strdup(cmd);
  if (!line) return;

  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

  // 2) Skip leading spaces
  char *p = line;
  while (*p && isspace((unsigned char)*p)) p++;
  if (*p == '\0') {
    free(line);
    return;
  }

  // 3) Split into argv
  char *argv[SH_MAX_ARGS];
  int argc = sh_split_argv(p, argv, SH_MAX_ARGS);
  if (argc <= 0) {
    free(line);
    return;
  }

  // 4) Built-ins
  if (strcmp(argv[0], "echo") == 0) {
    for (int i = 1; i < argc; i++) {
      if (i != 1) sh_printf(" ");
      sh_printf("%s", argv[i]);
    }
    sh_printf("\n");
    free(line);
    return;
  }

  if (strcmp(argv[0], "help") == 0) {
    sh_printf("Built-in commands:\n");
    sh_printf("  echo [args...]\n");
    sh_printf("  help\n");
    sh_printf("  exit\n");
    sh_printf("External programs:\n");
    sh_printf("  <prog> [args...], example: pal --skip\n");
    free(line);
    return;
  }

  if (strcmp(argv[0], "exit") == 0) {
    _exit(0);
  }

  // 5) External command: execve replaces current image
  char path[SH_MAX_PATH];
  const char *filename = sh_resolve_path(argv[0], path, sizeof(path));

  execvp(filename, argv);

  // If we reached here, execve failed
  // On some minimal libc, strerror may be missing, errno is still useful
  sh_printf("execve failed: %s, errno=%d\n", filename, errno);

  free(line);
}


void builtin_sh_run() {
  sh_banner();
  sh_prompt();

  // Make PATH available to execvp and Busybox printenv
  setenv("PATH", "/bin:/usr/bin", 1);

  while (1) {
    SDL_Event ev;
    if (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_KEYUP || ev.type == SDL_KEYDOWN) {
        const char *res = term->keypress(handle_key(&ev));
        if (res) {
          sh_handle_cmd(res);
          sh_prompt();
        }
      }
    }
    refresh_terminal();
  }
}
