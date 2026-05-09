#include "types.h"
#include "sound.h"
#include <stddef.h>

//uint32 soundtsoffs = 0;
//uint32 soundtsinc = 1;
//bool DMC_7bit = 0;
//EXPSOUND GameExpSound = {};
//int32 WaveHi[40000] = {};
//int32 Wave[2048+512] = {};
//int32 nesincsize=0;
uint8 *UNIFchrrama = 0;

void AddExState(void *v, uint32 s, int type, const char *desc) { }
void FCEU_CheatAddRAM(int s, uint32 A, uint8 *p) { }

#ifndef __ISA_NATIVE__
void *operator new(size_t size) {
	void *p = malloc(size);
	assert(p);
	return p;
}

void *operator new[](size_t size) {
	void *p = malloc(size);
	assert(p);
	return p;
}

void operator delete(void *p) noexcept {
	free(p);
}

void operator delete[](void *p) noexcept {
	free(p);
}

void operator delete(void *p, size_t) noexcept {
	free(p);
}

void operator delete[](void *p, size_t) noexcept {
	free(p);
}
#endif
