#ifndef KLIB_STDLIB_H__
#define KLIB_STDLIB_H__

#if defined(__ISA_NATIVE__) && !defined(__NATIVE_USE_KLIB__)
#include_next <stdlib.h>
#else
#include <klib.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

#endif

#endif
