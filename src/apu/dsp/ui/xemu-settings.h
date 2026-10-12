/* xemu settings stand-in for the DSP sources: interpreter, never the JIT. */
#ifndef XBOXRECOMP_DSP_SETTINGS_H
#define XBOXRECOMP_DSP_SETTINGS_H
#include <stdbool.h>
static const struct { struct { bool use_dsp_jit; } audio; } g_config = { { false } };
#endif
