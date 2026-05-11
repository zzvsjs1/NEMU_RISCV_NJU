#include <utils.h>

#ifdef CONFIG_IRINGBUF
/* Each slot stores the same fixed-size disassembly line used by itrace. */
static char iringbuf[CONFIG_IRINGBUF_SIZE][128];
static int iringbuf_next = 0;
static int iringbuf_count = 0;

void trace_iringbuf_record(const char *logbuf)
{
    if (logbuf == NULL || CONFIG_IRINGBUF_SIZE <= 0)
    {
        return;
    }

    /* Overwrite the oldest instruction once the ring is full. */
    snprintf(iringbuf[iringbuf_next], sizeof(iringbuf[iringbuf_next]), "%s", logbuf);
    iringbuf_next = (iringbuf_next + 1) % CONFIG_IRINGBUF_SIZE;
    if (iringbuf_count < CONFIG_IRINGBUF_SIZE)
    {
        iringbuf_count++;
    }
}

void trace_iringbuf_dump()
{
    if (iringbuf_count == 0)
    {
        return;
    }

    printf("Recent instructions:\n");
    /* Print oldest to newest; mark the most recent instruction as the fault site. */
    int start = (iringbuf_next - iringbuf_count + CONFIG_IRINGBUF_SIZE) % CONFIG_IRINGBUF_SIZE;
    for (int i = 0; i < iringbuf_count; i++)
    {
        int idx = (start + i) % CONFIG_IRINGBUF_SIZE;
        const char *mark = (idx == (iringbuf_next - 1 + CONFIG_IRINGBUF_SIZE) % CONFIG_IRINGBUF_SIZE) ? "--> " : "    ";
        printf("%s%s\n", mark, iringbuf[idx]);
    }
}
#else
void trace_iringbuf_record(const char *logbuf) {}
void trace_iringbuf_dump() {}
#endif
