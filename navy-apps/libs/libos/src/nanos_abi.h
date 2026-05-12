#ifndef NAVY_LIBOS_NANOS_ABI_H
#define NAVY_LIBOS_NANOS_ABI_H

#include <sys/dirent.h>
#include <sys/stat.h>

#include "nanos_syscall_abi.h"

/*
 * Translate a project-owned Nanos stat record into newlib's public struct stat.
 * The caller passes a libc struct, but the kernel never sees that libc layout.
 */
void nanos_stat_to_newlib(struct stat *dst, const NanosStat *src);

/*
 * Pack one or more fixed-size Nanos directory records into the variable-size
 * BSD dirent records consumed by this newlib readdir() implementation.
 */
int nanos_dirents_to_newlib(void *dst, int dst_len, const NanosDirent *src, int src_len);

#endif
