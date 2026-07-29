#ifndef __SYSCALL_H__
#define __SYSCALL_H__

enum
{
    SYS_exit,
    SYS_yield,
    SYS_open,
    SYS_read,
    SYS_write,
    SYS_kill,
    SYS_getpid,
    SYS_close,
    SYS_lseek,
    SYS_brk,
    SYS_fstat,
    SYS_time,
    SYS_signal,
    SYS_execve,
    SYS_fork,
    SYS_link,
    SYS_unlink,
    SYS_wait,
    SYS_times,
    SYS_gettimeofday,
    SYS_stat,
    SYS_getdents,
    SYS_mkdir,
    SYS_rmdir,
    SYS_rename,
    SYS_truncate,
    SYS_ftruncate,
    SYS_clock_gettime,

    /*
     * Keep the original ABI numbers above unchanged.  These calls are
     * deliberately appended so binaries built for the single-threaded kernel
     * continue to use exactly the same syscall numbers.
     */
    SYS_thread_create = 28,
    SYS_thread_exit = 29,
    SYS_thread_join = 30,
    SYS_thread_self = 31,
    SYS_thread_kill = 32,
    SYS_mutex_lock = 33,
    SYS_mutex_unlock = 34
};

#endif
