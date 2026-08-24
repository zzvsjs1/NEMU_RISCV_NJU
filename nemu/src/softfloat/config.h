#ifndef NEMU_SOFTFLOAT_CONFIG_H
#define NEMU_SOFTFLOAT_CONFIG_H

/*
 * SoftFloat needs the host byte order, not the guest byte order. Compiler
 * constants keep this integration independent of generated Spike headers.
 */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define WORDS_BIGENDIAN 1
#endif

#endif
