#ifndef NAVY_NDL_AUDIO_WRITE_H
#define NAVY_NDL_AUDIO_WRITE_H

typedef int (*NdlAudioWriteOnce)(int fd, const void *buffer, int length);
typedef void (*NdlAudioYield)(void);

int ndl_audio_write_all(int fd, const void *buffer, int length,
                        NdlAudioWriteOnce write_once,
                        NdlAudioYield yield_once);

#endif
