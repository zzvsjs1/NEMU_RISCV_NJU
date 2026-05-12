#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <time.h>
#include <errno.h>
#include "syscall.h"
#include "nanos_abi.h"

// helper macros
#define _concat(x, y) x##y
#define concat(x, y) _concat(x, y)
#define _args(n, list) concat(_arg, n) list
#define _arg0(a0, ...) a0
#define _arg1(a0, a1, ...) a1
#define _arg2(a0, a1, a2, ...) a2
#define _arg3(a0, a1, a2, a3, ...) a3
#define _arg4(a0, a1, a2, a3, a4, ...) a4
#define _arg5(a0, a1, a2, a3, a4, a5, ...) a5

// extract an argument from the macro array
#define SYSCALL _args(0, ARGS_ARRAY)
#define GPR1 _args(1, ARGS_ARRAY)
#define GPR2 _args(2, ARGS_ARRAY)
#define GPR3 _args(3, ARGS_ARRAY)
#define GPR4 _args(4, ARGS_ARRAY)
#define GPRx _args(5, ARGS_ARRAY)

// ISA-depedent definitions
#if defined(__ISA_X86__)
#define ARGS_ARRAY ("int $0x80", "eax", "ebx", "ecx", "edx", "eax")
#elif defined(__ISA_MIPS32__)
#define ARGS_ARRAY ("syscall", "v0", "a0", "a1", "a2", "v0")
#elif defined(__riscv)
#ifdef __riscv_e
#define ARGS_ARRAY ("ecall", "a5", "a0", "a1", "a2", "a0")
#else
#define ARGS_ARRAY ("ecall", "a7", "a0", "a1", "a2", "a0")
#endif
#elif defined(__ISA_AM_NATIVE__)
#define ARGS_ARRAY ("call *0x100000", "rdi", "rsi", "rdx", "rcx", "rax")
#elif defined(__ISA_X86_64__)
#define ARGS_ARRAY ("int $0x80", "rdi", "rsi", "rdx", "rcx", "rax")
#elif defined(__ISA_LOONGARCH32R__)
#define ARGS_ARRAY ("syscall 0", "a7", "a0", "a1", "a2", "a0")
#else
#error _syscall_ is not implemented
#endif

intptr_t _syscall_(intptr_t type, intptr_t a0, intptr_t a1, intptr_t a2)
{
    register intptr_t _gpr1 asm(GPR1) = type;
    register intptr_t _gpr2 asm(GPR2) = a0;
    register intptr_t _gpr3 asm(GPR3) = a1;
    register intptr_t _gpr4 asm(GPR4) = a2;
    register intptr_t ret asm(GPRx);
    asm volatile(SYSCALL : "=r"(ret) : "r"(_gpr1), "r"(_gpr2), "r"(_gpr3), "r"(_gpr4));
    return ret;
}

/*
 * Convert the kernel's small negative-error convention into libc's -1/errno
 * convention.  Nanos-lite usually returns plain -1, so fallback_errno supplies
 * the best local errno for that call.
 */
static int syscall_ret_errno(int ret, int fallback_errno)
{
    if (ret < 0)
    {
        errno = fallback_errno;
        return -1;
    }

    return ret;
}

void _exit(int status)
{
    _syscall_(SYS_exit, status, 0, 0);
    while (1)
        ;
}

int _open(const char *path, int flags, mode_t mode)
{
    return _syscall_(SYS_open, (intptr_t)path, (intptr_t)flags, (intptr_t)mode);
}

int _write(int fd, void *buf, size_t count)
{
    return _syscall_(SYS_write, (intptr_t)fd, (intptr_t)buf, (intptr_t)count);
}

void *_sbrk(intptr_t increment)
{
    extern char _end;
    static char *program_break = NULL;
    // First call, set to _end;

    if (program_break == NULL)
    {
        program_break = &_end;
    }

    char *old_break = program_break;
    char *new_break = old_break + increment;

    // Call SYS_brk

    if (_syscall_(SYS_brk, (intptr_t)new_break, 0, 0) == 0)
    {
        // Success
        program_break = new_break;
        return (void *)old_break;
    }
    else
    {
        // Fail
        return (void *)-1;
    }
}

int _read(int fd, void *buf, size_t count)
{
    return _syscall_(SYS_read, (intptr_t)fd, (intptr_t)buf, (intptr_t)count);
}

int _close(int fd)
{
    return _syscall_(SYS_close, (intptr_t)fd, (intptr_t)0, (intptr_t)0);
}

off_t _lseek(int fd, off_t offset, int whence)
{
    return _syscall_(SYS_lseek, (intptr_t)fd, (intptr_t)offset, (intptr_t)whence);
}

int _gettimeofday(struct timeval *tv, struct timezone *tz)
{
    return _syscall_(SYS_gettimeofday, (intptr_t)tv, (intptr_t)tz, (intptr_t)0);
}

int _execve(const char *fname, char *const argv[], char *const envp[])
{
    int ret = _syscall_(SYS_execve, (intptr_t)fname, (intptr_t)argv, (intptr_t)envp);

    if (ret < 0)
    {
        errno = -ret;
        return -1;
    }

    return 0; // execve success should not return
}

/*
 * Fill POSIX metadata for an open descriptor.
 */
int _fstat(int fd, struct stat *buf)
{
    NanosStat nanos;
    int ret;

    if (buf == NULL)
    {
        errno = EFAULT;
        return -1;
    }

    /*
     * Pass a NanosStat scratch object to the kernel instead of the caller's
     * struct stat.  This is the key libc/kernel split: Nanos-lite fills only the
     * stable syscall ABI, and this wrapper adapts it to newlib after the trap
     * returns.
     */
    ret = syscall_ret_errno(_syscall_(SYS_fstat, (intptr_t)fd, (intptr_t)&nanos, 0), EBADF);

    if (ret < 0)
    {
        return ret;
    }

    nanos_stat_to_newlib(buf, &nanos);
    return 0;
}

/*
 * Fill POSIX metadata for a pathname.
 */
int _stat(const char *fname, struct stat *buf)
{
    NanosStat nanos;
    int ret;

    if (buf == NULL)
    {
        errno = EFAULT;
        return -1;
    }

    /*
     * Keep pathname lookup in the kernel, but keep newlib's struct stat layout
     * in user space.  If newlib changes field sizes later, this translation
     * layer is the only place that should need to change.
     */
    ret = syscall_ret_errno(_syscall_(SYS_stat, (intptr_t)fname, (intptr_t)&nanos, 0), ENOENT);

    if (ret < 0)
    {
        return ret;
    }

    nanos_stat_to_newlib(buf, &nanos);
    return 0;
}

/*
 * Create a directory through the appended POSIX filesystem syscall.
 */
int _mkdir(const char *path, mode_t mode)
{
    return syscall_ret_errno(_syscall_(SYS_mkdir, (intptr_t)path, (intptr_t)mode, 0), EIO);
}

int _kill(int pid, int sig)
{
    _exit(-SYS_kill);
    return -1;
}

pid_t _getpid()
{
    _exit(-SYS_getpid);
    return 1;
}

pid_t _fork()
{
    assert(0);
    return -1;
}

pid_t vfork()
{
    assert(0);
    return -1;
}

int _link(const char *d, const char *n)
{
    assert(0);
    return -1;
}

/*
 * Remove a regular filesystem entry.
 */
int _unlink(const char *n)
{
    return syscall_ret_errno(_syscall_(SYS_unlink, (intptr_t)n, 0, 0), ENOENT);
}

/*
 * Rename a filesystem entry.
 */
int _rename(const char *old_path, const char *new_path)
{
    return syscall_ret_errno(_syscall_(SYS_rename, (intptr_t)old_path, (intptr_t)new_path, 0), EIO);
}

/*
 * Remove an empty directory.
 */
int rmdir(const char *path)
{
    return syscall_ret_errno(_syscall_(SYS_rmdir, (intptr_t)path, 0, 0), ENOTEMPTY);
}

/*
 * Resize a regular file by pathname.
 */
int truncate(const char *path, off_t length)
{
    return syscall_ret_errno(_syscall_(SYS_truncate, (intptr_t)path, (intptr_t)length, 0), EIO);
}

/*
 * Resize an open regular file.
 */
int ftruncate(int fd, off_t length)
{
    return syscall_ret_errno(_syscall_(SYS_ftruncate, (intptr_t)fd, (intptr_t)length, 0), EBADF);
}

/*
 * Provide the BSD/newlib getdents hook used by readdir().
 */
int getdents(int fd, void *buf, int count)
{
    NanosDirent nanos;
    int ret;
    int packed;

    if (buf == NULL || count <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * Request one kernel ABI record at a time.  This avoids losing directory
     * entries when a future libc dirent layout is larger than expected: Nanos-lite
     * advances its directory iterator only for the single record that libos is
     * about to repack into the caller's buffer.
     */
    ret = syscall_ret_errno(_syscall_(SYS_getdents, (intptr_t)fd, (intptr_t)&nanos, (intptr_t)sizeof(nanos)), EBADF);

    if (ret <= 0)
    {
        return ret;
    }

    packed = nanos_dirents_to_newlib(buf, count, &nanos, ret);

    if (packed < 0)
    {
        errno = EINVAL;
        return -1;
    }

    return packed;
}

pid_t _wait(int *status)
{
    assert(0);
    return -1;
}

clock_t _times(void *buf)
{
    assert(0);
    return 0;
}

int pipe(int pipefd[2])
{
    assert(0);
    return -1;
}

int dup(int oldfd)
{
    assert(0);
    return -1;
}

int dup2(int oldfd, int newfd)
{
    return -1;
}

unsigned int sleep(unsigned int seconds)
{
    assert(0);
    return -1;
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz)
{
    assert(0);
    return -1;
}

int symlink(const char *target, const char *linkpath)
{
    assert(0);
    return -1;
}

int ioctl(int fd, unsigned long request, ...)
{
    return -1;
}
