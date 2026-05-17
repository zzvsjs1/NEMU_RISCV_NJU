#ifndef KLIB_ASSERT_H__
#define KLIB_ASSERT_H__

#if defined(__ISA_NATIVE__) && !defined(__NATIVE_USE_KLIB__)
#include_next <assert.h>
#else
#include <klib.h>
#endif

#endif
