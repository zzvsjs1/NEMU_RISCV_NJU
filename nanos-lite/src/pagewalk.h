#ifndef NANOS_PAGEWALK_H__
#define NANOS_PAGEWALK_H__

#include <stdint.h>

void *nanos_pagewalk_lookup_page(void *root, uintptr_t vaddr);

#endif
