#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--child") == 0)
    {
        if (argc != 4 || strcmp(argv[0], "execvp-test") != 0 || strcmp(argv[2], "arg ok") != 0 || strcmp(argv[3], "tail") != 0)
        {
            printf("EXECVP_TEST: bad argv argc=%d argv0=%s\n", argc, argv[0]);
            return 2;
        }

        const char *value = getenv("EXECVP_TEST");
        if (value == NULL || strcmp(value, "child-env") != 0)
        {
            printf("EXECVP_TEST: bad env value=%s\n", value ? value : "(null)");
            return 3;
        }

        printf("EXECVP_TEST: PASS\n");
        return 0;
    }

    if (setenv("PATH", "/bin", 1) != 0 || setenv("EXECVP_TEST", "child-env", 1) != 0)
    {
        printf("EXECVP_TEST: setenv failed\n");
        return 1;
    }

    char *const child_argv[] = {"execvp-test", "--child", "arg ok", "tail", NULL};
    execvp("execvp-test", child_argv);

    printf("EXECVP_TEST: execvp failed errno=%d\n", errno);
    return 4;
}
