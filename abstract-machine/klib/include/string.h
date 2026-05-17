#ifndef KLIB_STRING_H__
#define KLIB_STRING_H__

#if defined(__ISA_NATIVE__) && !defined(__NATIVE_USE_KLIB__)
#include_next <string.h>
#else
#include <klib.h>
#endif

#endif
