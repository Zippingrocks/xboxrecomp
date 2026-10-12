/* Minimal stand-in for QEMU's osdep.h, enough for the xemu DSP sources. */
#ifndef XBOXRECOMP_DSP_OSDEP_H
#define XBOXRECOMP_DSP_OSDEP_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_free(p) free(p)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef likely
#define likely(x) (x)
#define unlikely(x) (x)
#endif
#ifndef QEMU_FALLTHROUGH
#define QEMU_FALLTHROUGH
#endif
#endif
