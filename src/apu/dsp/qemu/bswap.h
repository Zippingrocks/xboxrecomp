/* Little-endian load/store helpers used by the DSP interpreter. */
#ifndef XBOXRECOMP_DSP_BSWAP_H
#define XBOXRECOMP_DSP_BSWAP_H
#include <stdint.h>
#include <string.h>
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
#endif
