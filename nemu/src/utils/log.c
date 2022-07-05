#include <common.h>

extern uint64_t g_nr_guest_instr;
FILE *log_fp = NULL;

void init_log(const char *log_file) 
{
    if (log_fp && log_fp != stdout && log_fp != stderr)
    {
        fclose(log_fp);
        log_fp = NULL;
    }
  
    if (log_file) 
    {
        FILE *fp = fopen(log_file, "w");
        Assert(fp, "Can not open '%s'", log_file);
        log_fp = fp;
    }

    Log("Log is written to %s", log_file ? log_file : "null");
}

bool log_enable() 
{
  return MUXDEF(CONFIG_TRACE, (g_nr_guest_instr >= CONFIG_TRACE_START) &&
         (g_nr_guest_instr <= CONFIG_TRACE_END), false);
}
