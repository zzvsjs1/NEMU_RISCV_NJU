#include <generated/autoconf.h>

#ifdef CONFIG_RV64

#include "jit-rv64-internal.h"

#if RV64_JIT_ENABLED && defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/*
 * Optional Linux perf-map publication for generated RV64 native code.
 *
 * This lives outside the dispatch and emitted-code paths. Ordinary runs make
 * one cached option check during arena initialisation and perform no profiling
 * I/O. A perf map cannot describe unload times, so an arena reset truncates all
 * old symbols before native addresses are reused; jitdump is the appropriate
 * later format if historical generations need to remain distinguishable.
 */

#if RV64_JIT_ENABLED && defined(__linux__)

static FILE *rv64_jit_perf_map_file;
static char rv64_jit_perf_map_path[64];
static uint64_t rv64_jit_perf_map_generation;
static uint64_t rv64_jit_perf_map_generation_records;
static bool rv64_jit_perf_map_failed;
static bool rv64_jit_perf_map_reset_warning_logged;

/* Flush and close the process map without removing it; perf reads it by PID. */
static void jit_perf_map_close(void)
{
    if (rv64_jit_perf_map_file != NULL)
    {
        (void)fclose(rv64_jit_perf_map_file);
        rv64_jit_perf_map_file = NULL;
    }
}

/*
 * Disable only symbol publication after an I/O failure. Generated code and
 * cache ownership must never depend on an optional profiler side channel.
 */
static void jit_perf_map_fail(const char *operation, int error_number)
{
    if (error_number == 0)
    {
        error_number = EIO;
    }

    jit_perf_map_close();
    rv64_jit_perf_map_failed = true;
    Log("jit: perf map %s failed (%s); disable perf-map output", operation, strerror(error_number));
}

/* Create the conventional map that Linux perf discovers from the NEMU PID. */
void rv64_jit_perf_map_init(bool requested)
{
    if (!requested || rv64_jit_perf_map_file != NULL || rv64_jit_perf_map_failed)
    {
        return;
    }

    const int path_length = snprintf(rv64_jit_perf_map_path, sizeof(rv64_jit_perf_map_path), "/tmp/perf-%ld.map", (long)getpid());
    if (path_length < 0 || (size_t)path_length >= sizeof(rv64_jit_perf_map_path))
    {
        jit_perf_map_fail("path construction", ENAMETOOLONG);
        return;
    }

    /*
     * O_NOFOLLOW prevents a pre-existing symbolic link in the shared temporary
     * directory from redirecting NEMU's output. O_NONBLOCK keeps an unexpected
     * FIFO from stalling initialisation. Do not truncate until fstat() has
     * proved that a stale same-PID entry is an ordinary file owned only by this
     * user; PIDs can be reused while old perf maps intentionally remain.
     */
    const int fd = open(rv64_jit_perf_map_path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
    {
        jit_perf_map_fail("open", errno);
        return;
    }

    struct stat map_stat;
    if (fstat(fd, &map_stat) != 0)
    {
        const int saved_errno = errno;
        (void)close(fd);
        jit_perf_map_fail("file validation", saved_errno);
        return;
    }

    if (!S_ISREG(map_stat.st_mode) || map_stat.st_uid != geteuid() || map_stat.st_nlink != 1)
    {
        (void)close(fd);
        jit_perf_map_fail("unsafe existing file", EACCES);
        return;
    }

    /*
     * open(2)'s mode applies only to newly created files. Tighten a stale
     * same-PID map as well, which keeps the advertised permission contract
     * true across PID reuse.
     */
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0)
    {
        const int saved_errno = errno;
        (void)close(fd);
        jit_perf_map_fail("permission setup", saved_errno);
        return;
    }

    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0)
    {
        const int saved_errno = errno;
        (void)close(fd);
        jit_perf_map_fail("initial truncation", saved_errno);
        return;
    }

    rv64_jit_perf_map_file = fdopen(fd, "w");
    if (rv64_jit_perf_map_file == NULL)
    {
        const int saved_errno = errno;
        (void)close(fd);
        jit_perf_map_fail("stream setup", saved_errno);
        return;
    }

    /*
     * Every record is explicitly flushed after publication. Line buffering is
     * an additional safeguard for future callers and does not alter semantics
     * if the C library declines the request.
     */
    (void)setvbuf(rv64_jit_perf_map_file, NULL, _IOLBF, 0);
    rv64_jit_perf_map_generation = 1;
    (void)atexit(jit_perf_map_close);
    Log("jit: perf map = %s", rv64_jit_perf_map_path);
}

/*
 * Discard symbol records before the arena starts allocating from offset zero
 * again. Appending a second overlapping generation would let perf assign an old
 * name to new machine code because the text map has no unload record.
 */
void rv64_jit_perf_map_reset(void)
{
    if (rv64_jit_perf_map_file == NULL)
    {
        return;
    }

    if (rv64_jit_perf_map_generation_records != 0 && !rv64_jit_perf_map_reset_warning_logged)
    {
        /*
         * The rewritten file remains internally coherent for the new arena
         * generation, but perf-map has no timestamps with which to identify
         * samples collected before these native addresses were reused.
         */
        Log("jit: perf map arena reset drops old symbols; samples spanning "
            "the reset need jitdump for exact attribution");
        rv64_jit_perf_map_reset_warning_logged = true;
    }

    if (fflush(rv64_jit_perf_map_file) != 0)
    {
        jit_perf_map_fail("flush before reset", errno);
        return;
    }

    const int fd = fileno(rv64_jit_perf_map_file);
    if (fd < 0)
    {
        jit_perf_map_fail("descriptor lookup", errno);
        return;
    }

    if (ftruncate(fd, 0) != 0)
    {
        jit_perf_map_fail("truncate on arena reset", errno);
        return;
    }

    if (fseek(rv64_jit_perf_map_file, 0, SEEK_SET) != 0)
    {
        jit_perf_map_fail("rewind on arena reset", errno);
        return;
    }

    clearerr(rv64_jit_perf_map_file);
    rv64_jit_perf_map_generation++;
    rv64_jit_perf_map_generation_records = 0;
}

/* Publish one complete, successfully installed native region. */
void rv64_jit_perf_map_publish(const rv64_jit_block_t *block, const uint8_t *native_start, size_t native_size)
{
    if (rv64_jit_perf_map_file == NULL || block == NULL || native_start == NULL || native_size == 0)
    {
        return;
    }

    const uint32_t context = rv64_jit_cache_context_mix(block->satp, block->ifetch_state);
    const int written = fprintf(rv64_jit_perf_map_file,
                                "%" PRIxPTR " %zx "
                                "rv64_pc_%016" PRIx64 "_ctx_%08" PRIx32 "_n_%" PRIu32 "_g_%" PRIu64 "\n",
                                (uintptr_t)native_start, native_size, (uint64_t)block->pc, context, block->insn_count, rv64_jit_perf_map_generation);

    if (written < 0 || fflush(rv64_jit_perf_map_file) != 0)
    {
        jit_perf_map_fail("record write", errno);
        return;
    }

    rv64_jit_perf_map_generation_records++;
}

#else

/* Non-Linux or non-native builds retain link-compatible no-op hooks. */
void rv64_jit_perf_map_init(bool requested)
{
    (void)requested;
}

void rv64_jit_perf_map_reset(void)
{
}

void rv64_jit_perf_map_publish(const rv64_jit_block_t *block, const uint8_t *native_start, size_t native_size)
{
    (void)block;
    (void)native_start;
    (void)native_size;
}

#endif

#endif /* CONFIG_RV64 */
