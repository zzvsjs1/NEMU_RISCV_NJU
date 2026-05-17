#ifndef KLIB_STDIO_H__
#define KLIB_STDIO_H__

#if defined(__ISA_NATIVE__) && !defined(__NATIVE_USE_KLIB__)
#include_next <stdio.h>
#else
#include <klib.h>

#define EOF (-1)

#endif

#endif
