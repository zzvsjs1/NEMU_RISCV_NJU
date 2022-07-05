#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>

// this should be enough
static char buf[65536] = {};
static char code_buf[65536 + 128] = {}; // a little larger than `buf`
static char *code_format =
"#include <stdio.h>\n"
"#include <stdint.h>\n"
"int main()"
"{"
"    uint32_t result = %s; "
"    printf(\"%%u\", result); "
"    return 0; "
"}";

static size_t bufSize = 0;

uint64_t nextInt(const uint64_t low, const uint64_t height)
{
    return rand() % (height + 1 - low) + low;
}

static void genNum()
{
    bufSize += sprintf(buf + bufSize, "%" PRIu64 "u", nextInt(0, 1024));
}

static void gen(const char ch)
{
    buf[bufSize++] = ch;
}

static void genOp()
{
    char ch = '\0';
    switch (nextInt(0, 7))
    {
    case 0:
        ch = '+';
        break;
    case 1:
        ch = '-';
        break;
    case 2:
        ch = '*';
        break;
    case 3:
        ch = '/';
        break;
    case 4:
        ch = '%';
        break;
    case 5:
        ch = '&';
        break;
    case 6:
        ch = '|';
        break;
    case 7:
        ch = '^';
        break;
    default:
        break;
    }

    buf[bufSize++] = ch;
}

static void next_unary_expr()
{
    if (nextInt(0, 1) == 0)
    {
        buf[bufSize++] = '-';
    }
}

static void gen_rand_expr()
{
    if (bufSize > 1024 * 3)
    {
        return;
    }

    switch (nextInt(0, 2))
    {
    case 0:
        next_unary_expr();
        genNum();
        break;
    case 1:
        next_unary_expr();
        gen('(');
        gen_rand_expr();
        gen(')');
        break;
    case 2:
        gen_rand_expr();
        genOp();
        gen_rand_expr();
        break;
    default:
        assert(0);
    }
}

int main(int argc, char *argv[]) 
{
    int seed = time(NULL);
    srand(seed);
    size_t loop = 1;

    if (argc > 1) 
    {
        sscanf(argv[1], "%zu", &loop);
    }

    for (size_t i = 0; i < loop;) 
    {
        bufSize = 0;
        gen_rand_expr(0);
        buf[bufSize] = '\0';

        sprintf(code_buf, code_format, buf);

        FILE *fp = fopen("/tmp/.code.c", "w");
        assert(fp != NULL);
        fputs(code_buf, fp);
        fclose(fp);

        int ret = system("gcc -std=c17 -Werror=div-by-zero -Werror /tmp/.code.c -o /tmp/.expr");
        if (ret != 0) 
        {
            continue;
        }

        fp = popen("/tmp/.expr", "r");
        assert(fp != NULL);

        int result;
        fscanf(fp, "%u", &result);
        pclose(fp);

        printf("%u %s\n", result, buf);

        ++i;
    }

    return 0;
}
