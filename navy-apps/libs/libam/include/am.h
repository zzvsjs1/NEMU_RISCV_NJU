#ifndef __AM_H__
#define __AM_H__

// Navy apps include the normal AbstractMachine headers, but resolve the
// platform hook through navy.h. This keeps first-party Navy glue visible while
// preserving the common AM-facing API used by tests and demos.
#define ARCH_H "navy.h"
#include "am-origin.h"

#endif
