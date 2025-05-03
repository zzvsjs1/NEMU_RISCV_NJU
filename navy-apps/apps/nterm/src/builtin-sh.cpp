#include <nterm.h>
#include <stdarg.h>
#include <unistd.h>
#include <SDL.h>

char handle_key(SDL_Event *ev);

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

static void sh_handle_cmd(const char *cmd) 
{
    // 1. Make a mutable copy and strip trailing newline
    char *line = strdup(cmd);
    const size_t len = strlen(line);

    if (len > 0 && line[len - 1] == '\n') 
    {
        line[len - 1] = '\0';
    }

    // 2. Tokenize on whitespace
    char *saveptr = NULL;
    char *token = strtok_r(line, " \t", &saveptr);

    if (!token) 
    {
        free(line);
        return;  // empty line, just return
    }

    // 3. Handle 'echo' command
    if (strcmp(token, "echo") == 0) 
    {
        char *arg;
        int first = 1;
        while ((arg = strtok_r(NULL, " \t", &saveptr))) {
            if (!first) {
                sh_printf(" ");  // space between args
            }

            sh_printf("%s", arg);
            first = 0;
        }
        
        sh_printf("\n");
    }
    // 4. Unknown command fallback
    else 
    {
        sh_printf("Unknown command: %s\n", token);
    }

    free(line);
}

void builtin_sh_run() {
  sh_banner();
  sh_prompt();

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
