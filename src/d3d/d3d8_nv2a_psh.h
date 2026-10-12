/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 xboxrecomp contributors
 * NV2A register combiners from raw PGRAPH words (port of xemu glsl/psh.c).
 */
#ifndef D3D8_NV2A_PSH_H
#define D3D8_NV2A_PSH_H

#include <stdint.h>
#include <windows.h>

typedef struct NV2APshRegs {
    uint32_t combiner_control;      /* NV097_SET_COMBINER_CONTROL */
    uint32_t shader_stage_program;  /* NV097_SET_SHADER_STAGE_PROGRAM */
    uint32_t other_stage_input;     /* NV097_SET_SHADER_OTHER_STAGE_INPUT */
    uint32_t final0, final1;        /* NV097_SET_COMBINER_SPECULAR_FOG_CW0/1 */
    uint32_t rgb_in[8], rgb_out[8]; /* NV097_SET_COMBINER_COLOR_ICW/OCW */
    uint32_t alpha_in[8], alpha_out[8];
    uint32_t factor0[8], factor1[8];/* NV097_SET_COMBINER_FACTOR0/1 (ARGB) */
    uint32_t final_factor0, final_factor1; /* NV097_SET_SPECULAR_FOG_FACTOR */
    uint32_t fog_color;             /* NV097_SET_FOG_COLOR */
    uint32_t alpha_test, alpha_func, alpha_ref;
    float bump_mat[4][4];           /* NV097_SET_TEXTURE_SET_BUMP_ENV_MAT */
    float bump_scale[4], bump_offset[4];
    uint32_t cube_mask;             /* stages with a cube texture bound */
} NV2APshRegs;

/* NULL disables (draws fall back to the fixed-function pixel shader). */
void d3d8_nv2a_psh_set(const NV2APshRegs *regs);
/* Binds the combiner shader for the next draw; FALSE if inactive. */
BOOL d3d8_nv2a_psh_prepare_draw(void);
/* Cube texture for a stage: faces +X,-X,+Y,-Y,+Z,-Z, size x size A8R8G8B8.
 * key 0 unbinds; a key already cached needs no pixels. */
int d3d8_nv2a_psh_set_cube(int stage, const uint32_t *argb_faces, uint32_t size, uint64_t key);

#endif
