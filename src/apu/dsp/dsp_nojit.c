/*
 * The xemu DSP JIT depends on an external dsp56300 recompiler. The
 * standalone build always uses the interpreter; these definitions satisfy
 * dsp.c's engine switch.
 */
#include "dsp_internal.h"

const DSPOps jit_dsp_ops = { 0 };

void dsp_jit_init(DSPState *dsp)
{
    dsp_c_init(dsp);
}
