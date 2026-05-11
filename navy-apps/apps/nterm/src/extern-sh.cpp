#include <nterm.h>
#include <fcntl.h>
#include <unistd.h>

static int read_fd, write_fd, nterm_to_app[2], app_to_nterm[2]; // file desc

char handle_key(const char *buf);

static void poll_terminal()
{
    static char buf[4096];
    int nread = read(read_fd, buf, sizeof(buf));
    if (nread > 0)
    {
        term->write(buf, nread);
    }
}

static void fork_child(const char *nterm_proc)
{
    const char *argv[] = {
        nterm_proc,
        NULL,
    };
    // Child apps only learn the terminal size through environment variables;
    // there is no termios/ioctl path in this Navy stack.  Keep these values tied
    // to the live Terminal grid so external apps follow the current display mode.
    char env_lines[32];
    sprintf(env_lines, "LINES=%d", term->h);
    char env_columns[32];
    sprintf(env_columns, "COLUMNS=%d", term->w);
    const char *envp[] = {
        env_lines,
        env_columns,
        "TERM=ansi",
        NULL};

    // Two unidirectional pipes emulate the stdin/stdout sides of a terminal.
    // The child sees only file descriptors 0, 1, and 2; NTerm keeps the other
    // ends and polls them from the graphical event loop.
    assert(0 == pipe(nterm_to_app));
    assert(0 == pipe(app_to_nterm));
    read_fd = app_to_nterm[0];
    write_fd = nterm_to_app[1];

    int flags = fcntl(read_fd, F_GETFL, 0);
    fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

    int stdin_fd = dup(0), stdout_fd = dup(1), stderr_fd = dup(2);

    // vfork() shares the address space until execve(), so only descriptor
    // redirection is prepared before the child starts. The parent restores its
    // original stdio immediately after the fork boundary.
    dup2(nterm_to_app[0], 0);
    dup2(app_to_nterm[1], 1);
    dup2(app_to_nterm[1], 2);

    pid_t p = vfork();
    if (p == 0)
    {
        execve(argv[0], (char **)argv, (char **)envp);
        assert(0);
    }
    else
    {
        dup2(stdin_fd, 0);
        dup2(stdout_fd, 1);
        dup2(stderr_fd, 2);
        close(stdin_fd);
        close(stdout_fd);
        close(stderr_fd);
    }
}

void extern_app_run(const char *app_path)
{
    int elapse = -1, ntick = 0, last_k = 0;

    fork_child(app_path); // fork the child process and setup fds

    while (1)
    {
        poll_terminal();
        char buf[256], *p = buf, ch;
        while ((ch = getc(stdin)) != -1)
        {
            *p++ = ch;
            if (ch == '\n')
                break;
        }
        *p = '\0';

        if (buf[0] == 'k')
        {
            const char *res = term->keypress(handle_key(buf + 1));
            if (res)
            {
                write(write_fd, res, strlen(res));
            }
        }

        refresh_terminal();
    }

    assert(0);
}
