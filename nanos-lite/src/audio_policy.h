#ifndef __NANOS_AUDIO_POLICY_H__
#define __NANOS_AUDIO_POLICY_H__

#include <stddef.h>

/*
 * Return the largest prefix that an MT /dev/sb write can enqueue immediately.
 * The caller takes one device-status snapshot and must return to user mode
 * instead of waiting on the sole emulated hart when this function returns zero.
 */
size_t mt_audio_write_count(size_t capacity, size_t used, size_t requested);

#endif
