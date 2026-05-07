#ifndef __NAVY_H__
#define __NAVY_H__

// Navy uses the host C library surface exposed by nanos-lite/newlib rather
// than real device registers in this libam shim. Platform-specific device
// behaviour is filled in through ioe.c where available.
#include <stdio.h>
#include <stdlib.h>

#endif
