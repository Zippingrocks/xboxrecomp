/*
 * NV2A Kelvin (NV097) pushbuffer -> D3D8 layer translator.
 *
 * Method numbers, vertex-program microcode fields and texture formats follow
 * xemu (hw/xbox/nv2a/nv2a_regs.h, pgraph/vsh_regs.h, pgraph/glsl/vsh-prog.c,
 * pgraph/glsl/vsh-ff.c). Vertex processing happens on the CPU and the result
 * is submitted as pre-transformed XYZRHW geometry, which is what the NV2A
 * rasteriser consumes anyway: a vertex program's oPos is already in screen
 * space (the XDK appends the viewport transform), and the fixed-function
 * path is the composite matrix followed by the perspective divide.
 *
 * Deliberately approximate: register combiners are reduced to
 * texture-modulates-diffuse, lighting is not evaluated, and culling is left
 * off. Everything else aims to be exact.
 */

#include "nv2a_kelvin.h"
#include "../d3d/d3d8_xbox.h"
#include "../d3d/d3d8_nv2a_psh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);

/* Guest physical address -> host pointer. Contiguous allocations in this
 * runtime are identity-mapped; the 0x80000000 mirror and DMA-context high
 * bits are masked off. */
uintptr_t g_kelvin_mem_base; /* nonzero: offline replay of a RAM image */
/* getenv is a locked scan of the environment block and sat on the per-draw
 * (and per-vertex) path; debug toggles are read once and cached here. */
static const char *kenv(const char *name)
{
    static struct { const char *name; const char *val; } cache[32];
    static int used;
    for (int i = 0; i < used; ++i)
        if (cache[i].name == name || !strcmp(cache[i].name, name)) return cache[i].val;
    {
        const char *v = getenv(name);
        if (used < 32) { cache[used].name = name; cache[used].val = v; used++; }
        return v;
    }
}

static const uint8_t *guest_ptr(uint32_t phys)
{
    return (const uint8_t *)((uintptr_t)(phys & 0x07FFFFFFu) +
                             (g_kelvin_mem_base ? g_kelvin_mem_base : (uintptr_t)g_xbox_mem_offset));
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ------------------------------------------------------------------ */
/* Method numbers (xemu nv2a_regs.h)                                   */
/* ------------------------------------------------------------------ */
#define M_SURFACE_CLIP_H        0x0200
#define M_SURFACE_CLIP_V        0x0204
#define M_CONTEXT_DMA_A         0x0184
#define M_CONTEXT_DMA_B         0x0188
#define M_ALPHA_TEST_ENABLE     0x0300
#define M_BLEND_ENABLE          0x0304
#define M_CULL_FACE_ENABLE      0x0308
#define M_DEPTH_TEST_ENABLE     0x030C
#define M_ALPHA_FUNC            0x033C
#define M_ALPHA_REF             0x0340
#define M_BLEND_SFACTOR         0x0344
#define M_BLEND_DFACTOR         0x0348
#define M_BLEND_EQUATION        0x0350
#define M_DEPTH_FUNC            0x0354
#define M_COLOR_MASK            0x0358
#define M_DEPTH_MASK            0x035C
#define M_CLIP_MIN              0x0394
#define M_CLIP_MAX              0x0398
#define M_COMPOSITE_MATRIX      0x0680 /* 16 floats */
#define M_VIEWPORT_OFFSET       0x0A20 /* 4 floats */
#define M_VIEWPORT_SCALE        0x0AF0 /* 4 floats */
#define M_TRANSFORM_PROGRAM     0x0B00 /* 32 dwords */
#define M_TRANSFORM_CONSTANT    0x0B80 /* 32 dwords */
#define M_VERTEX_ARRAY_OFFSET   0x1720 /* 16 attributes */
#define M_VERTEX_ARRAY_FORMAT   0x1760 /* 16 attributes */
#define M_BEGIN_END             0x17FC
#define M_ARRAY_ELEMENT16       0x1800
#define M_ARRAY_ELEMENT32       0x1808
#define M_DRAW_ARRAYS           0x1810
#define M_INLINE_ARRAY          0x1818
#define M_VERTEX_DATA2F         0x1880 /* 16 attrs x 2 floats */
#define M_VERTEX_DATA2S         0x1900 /* 16 attrs x 1 dword */
#define M_VERTEX_DATA4UB        0x1940 /* 16 attrs x 1 dword */
#define M_VERTEX_DATA4S         0x1980 /* 16 attrs x 2 dwords */
#define M_VERTEX_DATA4F         0x1A00 /* 16 attrs x 4 floats */
#define M_TEXTURE_BASE          0x1B00 /* +0x40 per stage */
#define M_ZSTENCIL_CLEAR_VALUE  0x1D8C
#define M_COLOR_CLEAR_VALUE     0x1D90
#define M_CLEAR_SURFACE         0x1D94
#define M_CLEAR_RECT_H          0x1D98
#define M_CLEAR_RECT_V          0x1D9C
#define M_TRANSFORM_EXEC_MODE   0x1E94
#define M_TRANSFORM_PROG_LOAD   0x1E9C
#define M_TRANSFORM_PROG_START  0x1EA0
#define M_TRANSFORM_CONST_LOAD  0x1EA4
#define M_TRANSFORM_DATA        0x1E80 /* four floats for vertex-state shaders */
#define M_LAUNCH_TRANSFORM      0x1E90

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
#define PROGRAM_SLOTS  136
#define CONST_ROWS     192
#define MAX_VERTS      65536
#define ATTR_DIFFUSE   3

typedef struct { float v[4]; } Vec4;

typedef struct {
    uint32_t offset, format, address, control0, control1, filter, image_rect, palette;
} TexStage;

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse, specular;
    float tex[4][4];   /* s/q, t/q, r/q, 1 (r for cube maps) */
} OutVertex;

static struct {
    int initialized;

    /* vertex program */
    uint32_t program[PROGRAM_SLOTS][4];
    uint32_t program_load;
    uint32_t program_start;
    int program_mode;               /* 1 = program, 0 = fixed function */
    float constants[CONST_ROWS][4];
    uint32_t const_load;
    float transform_data[4];
    float composite[4][4];
    float viewport_offset[4];
    float viewport_scale[4];
    float clip_min, clip_max;

    /* vertex data */
    uint32_t attr_format[16];
    uint32_t attr_offset[16];
    float attr_const[16][4];

    /* current primitive */
    uint32_t prim;
    int in_begin;
    uint32_t inline_data[MAX_VERTS * 4];
    uint32_t inline_count;
    uint32_t *indices;
    uint32_t index_count, index_cap;
    uint32_t array_start, array_count;
    int have_arrays;

    /* raster state */
    uint32_t blend_enable, sfactor, dfactor, blend_eq;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t color_mask;
    uint32_t clip_h, clip_v;
    uint32_t dma_a, dma_b;

    /* clear */
    uint32_t clear_color, clear_zstencil, clear_rect_h, clear_rect_v;

    TexStage tex[4];
    NV2APshRegs psh;                /* register combiner state */

    KelvinStats stats;
    uint32_t frame;
} K;

/* Program decode cache is reset with the rest of Kelvin state.  Keep its
 * storage declaration above kelvin_init so the reset is visible to C. */
static uint8_t g_vdec_ok[PROGRAM_SLOTS];

static uint8_t g_vdec_ok[PROGRAM_SLOTS];   /* decoded-slot validity (defined with the decoder below) */

void kelvin_init(void)
{
    memset(&K, 0, sizeof(K));
    memset(g_vdec_ok, 0, sizeof(g_vdec_ok));
    for (int i = 0; i < 16; ++i) {
        K.attr_const[i][0] = K.attr_const[i][1] = K.attr_const[i][2] = 0.0f;
        K.attr_const[i][3] = 1.0f;
        K.attr_format[i] = 0x02; /* float, size 0: disabled */
    }
    K.attr_const[ATTR_DIFFUSE][0] = K.attr_const[ATTR_DIFFUSE][1] = 1.0f;
    K.attr_const[ATTR_DIFFUSE][2] = K.attr_const[ATTR_DIFFUSE][3] = 1.0f;
    K.color_mask = 0x01010101u;
    K.depth_func = 0x203; /* GL_LEQUAL */
    K.depth_mask = 1;
    K.clip_max = 16777215.0f;
    K.sfactor = 1; K.dfactor = 0;
    K.clip_h = 640u << 16; K.clip_v = 480u << 16;
    K.initialized = 1;
    fprintf(stderr, "[KELVIN] translator initialized\n");
}

void kelvin_get_stats(KelvinStats *out) { if (out) *out = K.stats; }

/* ------------------------------------------------------------------ */
/* Vertex program interpreter (field layout from xemu vsh-prog.c)      */
/* ------------------------------------------------------------------ */
static uint32_t vsh_field(const uint32_t *t, int dw, int bit, int len)
{
    return (t[dw] >> bit) & ((1u << len) - 1u);
}

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

typedef struct {
    Vec4 r[13];      /* R0..R11, R12 = oPos */
    Vec4 o[16];      /* outputs; o[0] mirrors R12 */
    int a0;
    uint32_t written; /* bit per output register the program wrote */
} VshState;

/* Decoded program slot. Field extraction per instruction per vertex was most
 * of the translator's time; slots are decoded once and invalidated when the
 * title uploads new program words (see M_TRANSFORM_PROGRAM). */
typedef struct { uint8_t mux, reg, neg, vin, rel, sw[4]; uint8_t cidx; } VSrc;
typedef struct {
    uint8_t mac, ilu, out_r, mac_mask, ilu_mask, o_mask, orb, o_addr, o_mux, fin;
    VSrc src[3];
} VDec;
static VDec g_vdec[PROGRAM_SLOTS];

/* NV2A vertex-program constant addresses use the Xbox/D3D signed encoding:
 * the upper three bits select a 32-register bank spanning c[-96]..c[95].
 * Keep the interpreter's storage indexed as 0..191, matching Xemu's
 * VSH_D3DSCM_CORRECTION conversion.  Treating the encoded byte as a direct
 * array index selects unrelated constants for most programs (notably the
 * main-menu material/highlight shaders). */
static uint8_t vsh_const_index(uint8_t encoded)
{
    int idx = ((((int)(encoded >> 5) & 7) - 3) * 32)
              + ((int)encoded & 31) + 96;
    return (uint8_t)idx;
}

static void vdec_src(const uint32_t *t, int which, int scalar_c, VSrc *o)
{
    uint32_t sw[4];
    if (which == 0) {
        o->mux = (uint8_t)vsh_field(t, 2, 26, 2); o->reg = (uint8_t)vsh_field(t, 2, 28, 4);
        o->neg = (uint8_t)vsh_field(t, 1, 8, 1);
        sw[0] = vsh_field(t, 1, 6, 2); sw[1] = vsh_field(t, 1, 4, 2);
        sw[2] = vsh_field(t, 1, 2, 2); sw[3] = vsh_field(t, 1, 0, 2);
    } else if (which == 1) {
        o->mux = (uint8_t)vsh_field(t, 2, 11, 2); o->reg = (uint8_t)vsh_field(t, 2, 13, 4);
        o->neg = (uint8_t)vsh_field(t, 2, 25, 1);
        sw[0] = vsh_field(t, 2, 23, 2); sw[1] = vsh_field(t, 2, 21, 2);
        sw[2] = vsh_field(t, 2, 19, 2); sw[3] = vsh_field(t, 2, 17, 2);
    } else {
        o->mux = (uint8_t)vsh_field(t, 3, 28, 2);
        o->reg = (uint8_t)((vsh_field(t, 2, 0, 2) << 2) | vsh_field(t, 3, 30, 2));
        o->neg = (uint8_t)vsh_field(t, 2, 10, 1);
        sw[0] = vsh_field(t, 2, 8, 2); sw[1] = vsh_field(t, 2, 6, 2);
        sw[2] = vsh_field(t, 2, 4, 2); sw[3] = vsh_field(t, 2, 2, 2);
        if (scalar_c) sw[1] = sw[2] = sw[3] = sw[0];
    }
    for (int i = 0; i < 4; ++i) o->sw[i] = (uint8_t)sw[i];
    o->vin = (uint8_t)vsh_field(t, 1, 9, 4);
    o->cidx = vsh_const_index((uint8_t)vsh_field(t, 1, 13, 8));
    o->rel = (uint8_t)vsh_field(t, 3, 1, 1);
}

static const VDec *vdec(uint32_t slot)
{
    VDec *d = &g_vdec[slot];
    if (!g_vdec_ok[slot]) {
        const uint32_t *t = K.program[slot];
        uint32_t ilu = vsh_field(t, 1, 25, 3);
        d->mac = (uint8_t)vsh_field(t, 1, 21, 4);
        d->ilu = (uint8_t)ilu;
        vdec_src(t, 0, 0, &d->src[0]);
        vdec_src(t, 1, 0, &d->src[1]);
        vdec_src(t, 2, ilu >= ILU_RCP && ilu <= ILU_LOG, &d->src[2]);
        d->out_r = (uint8_t)vsh_field(t, 3, 20, 4);
        d->mac_mask = (uint8_t)vsh_field(t, 3, 24, 4);
        d->ilu_mask = (uint8_t)vsh_field(t, 3, 16, 4);
        d->o_mask = (uint8_t)vsh_field(t, 3, 12, 4);
        d->orb = (uint8_t)vsh_field(t, 3, 11, 1);
        /* Keep all eight address bits.  Output-register writes use the low
         * nibble, but vertex-state programs use the full encoded address to
         * write c[-96]..c[95].  Truncating here redirected c105/c106 writes
         * to c9/c10 and left the menu's reflection vectors at zero. */
        d->o_addr = (uint8_t)vsh_field(t, 3, 3, 8);
        d->o_mux = (uint8_t)vsh_field(t, 3, 2, 1);
        d->fin = (uint8_t)vsh_field(t, 3, 0, 1);
        g_vdec_ok[slot] = 1;
    }
    return d;
}

static Vec4 vsh_src(const VSrc *d, const Vec4 *v, const VshState *s)
{
    const float *src;
    static const float zero[4] = { 0, 0, 0, 0 };
    Vec4 out;
    if (d->mux == 1) src = (d->reg == 12) ? s->o[0].v : s->r[d->reg < 12 ? d->reg : 0].v;
    else if (d->mux == 2) src = v[d->vin].v;
    else if (d->mux == 3) {
        int idx = (int)d->cidx;
        if (d->rel) idx += s->a0;
        if (idx < 0) idx = 0;
        if (idx >= CONST_ROWS) idx = CONST_ROWS - 1;
        src = K.constants[idx];
    } else src = zero;
    if (d->neg) {
        out.v[0] = -src[d->sw[0]]; out.v[1] = -src[d->sw[1]];
        out.v[2] = -src[d->sw[2]]; out.v[3] = -src[d->sw[3]];
    } else {
        out.v[0] = src[d->sw[0]]; out.v[1] = src[d->sw[1]];
        out.v[2] = src[d->sw[2]]; out.v[3] = src[d->sw[3]];
    }
    return out;
}

static float vmul(float a, float b)
{
    /* hardware: anything times zero is zero, even inf/NaN */
    if (a == 0.0f || b == 0.0f) return 0.0f;
    return a * b;
}

static void write_masked(Vec4 *dst, const Vec4 *val, uint32_t mask)
{
    for (int i = 0; i < 4; ++i)
        if (mask & (8u >> i)) dst->v[i] = val->v[i];
}

static void vsh_run_from(uint32_t program_start, const Vec4 *v, VshState *s,
                         int context_writes)
{
    memset(s, 0, sizeof(*s));
    s->o[0].v[3] = 1.0f;
    s->o[3].v[3] = 1.0f; s->o[4].v[3] = 1.0f;
    for (int i = 9; i < 13; ++i) s->o[i].v[3] = 1.0f;

    for (uint32_t slot = program_start; slot < PROGRAM_SLOTS; ++slot) {
        const VDec *d = vdec(slot);
        uint32_t mac = d->mac;
        uint32_t ilu = d->ilu;
        Vec4 a = vsh_src(&d->src[0], v, s);
        Vec4 b = vsh_src(&d->src[1], v, s);
        Vec4 c = vsh_src(&d->src[2], v, s);
        Vec4 mr, ir;
        int mac_valid = 0, ilu_valid = 0, arl = 0;
        memset(&mr, 0, sizeof(mr)); memset(&ir, 0, sizeof(ir));

        switch (mac) {
        case MAC_MOV: mr = a; mac_valid = 1; break;
        case MAC_MUL: for (int i = 0; i < 4; ++i) mr.v[i] = vmul(a.v[i], b.v[i]); mac_valid = 1; break;
        case MAC_ADD: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] + c.v[i]; mac_valid = 1; break;
        case MAC_MAD: for (int i = 0; i < 4; ++i) mr.v[i] = vmul(a.v[i], b.v[i]) + c.v[i]; mac_valid = 1; break;
        case MAC_DP3: { float d = vmul(a.v[0], b.v[0]) + vmul(a.v[1], b.v[1]) + vmul(a.v[2], b.v[2]);
                        for (int i = 0; i < 4; ++i) mr.v[i] = d; mac_valid = 1; } break;
        case MAC_DPH: { float d = vmul(a.v[0], b.v[0]) + vmul(a.v[1], b.v[1]) + vmul(a.v[2], b.v[2]) + b.v[3];
                        for (int i = 0; i < 4; ++i) mr.v[i] = d; mac_valid = 1; } break;
        case MAC_DP4: { float d = vmul(a.v[0], b.v[0]) + vmul(a.v[1], b.v[1]) + vmul(a.v[2], b.v[2]) + vmul(a.v[3], b.v[3]);
                        for (int i = 0; i < 4; ++i) mr.v[i] = d; mac_valid = 1; } break;
        case MAC_DST: mr.v[0] = 1.0f; mr.v[1] = vmul(a.v[1], b.v[1]); mr.v[2] = a.v[2]; mr.v[3] = b.v[3]; mac_valid = 1; break;
        case MAC_MIN: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; mac_valid = 1; break;
        case MAC_MAX: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; mac_valid = 1; break;
        case MAC_SLT: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; mac_valid = 1; break;
        case MAC_SGE: for (int i = 0; i < 4; ++i) mr.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; mac_valid = 1; break;
        case MAC_ARL: arl = 1; break;
        default: break;
        }

        switch (ilu) {
        case ILU_MOV: ir = c; ilu_valid = 1; break;
        case ILU_RCP: { float r = 1.0f / c.v[0]; for (int i = 0; i < 4; ++i) ir.v[i] = r; ilu_valid = 1; } break;
        case ILU_RCC: { float r = 1.0f / c.v[0];
                        if (r > 0.0f) r = r < 5.42101e-20f ? 5.42101e-20f : r > 1.84467e+19f ? 1.84467e+19f : r;
                        else r = r > -5.42101e-20f ? -5.42101e-20f : r < -1.84467e+19f ? -1.84467e+19f : r;
                        for (int i = 0; i < 4; ++i) ir.v[i] = r; ilu_valid = 1; } break;
        case ILU_RSQ: { float x = fabsf(c.v[0]); float r = x == 0.0f ? INFINITY : 1.0f / sqrtf(x);
                        for (int i = 0; i < 4; ++i) ir.v[i] = r; ilu_valid = 1; } break;
        case ILU_EXP: { float x = c.v[0]; ir.v[0] = exp2f(floorf(x)); ir.v[1] = x - floorf(x);
                        ir.v[2] = exp2f(x); ir.v[3] = 1.0f; ilu_valid = 1; } break;
        case ILU_LOG: { float x = fabsf(c.v[0]);
                        if (x == 0.0f) { ir.v[0] = -INFINITY; ir.v[1] = 1.0f; ir.v[2] = -INFINITY; }
                        else { float e = floorf(log2f(x)); ir.v[0] = e; ir.v[1] = x / exp2f(e); ir.v[2] = log2f(x); }
                        ir.v[3] = 1.0f; ilu_valid = 1; } break;
        case ILU_LIT: { float x = c.v[0] > 0 ? c.v[0] : 0, y = c.v[1] > 0 ? c.v[1] : 0, w = c.v[3];
                        if (w < -127.9961f) w = -127.9961f; if (w > 127.9961f) w = 127.9961f;
                        ir.v[0] = 1.0f; ir.v[1] = x; ir.v[2] = x > 0 ? exp2f(w * log2f(y)) : 0.0f; ir.v[3] = 1.0f;
                        ilu_valid = 1; } break;
        default: break;
        }

        uint32_t out_r = d->out_r;
        uint32_t mac_mask = d->mac_mask;
        uint32_t ilu_mask = d->ilu_mask;
        uint32_t o_mask = d->o_mask;
        uint32_t orb = d->orb;
        uint32_t o_addr = d->o_addr;
        uint32_t o_mux = d->o_mux;

        if (arl) s->a0 = (int)floorf(a.v[0] + 0.001f);

        if (mac_valid) {
            uint32_t m = mac_mask;
            if (ilu != ILU_NOP && out_r == 1) m = 0; /* paired MAC to R1 is dropped */
            if (m) {
                if (out_r == 12) write_masked(&s->o[0], &mr, m);
                else if (out_r < 12) write_masked(&s->r[out_r], &mr, m);
            }
            if (o_mux == 0 && o_mask) {
                if (orb == 1) {
                    uint32_t output = o_addr & 0xFu;
                    write_masked(&s->o[output], &mr, o_mask);
                    s->written |= 1u << output;
                } else if (context_writes) {
                    uint32_t constant = vsh_const_index((uint8_t)o_addr);
                    if (constant < CONST_ROWS)
                        write_masked((Vec4 *)K.constants[constant], &mr, o_mask);
                }
            }
        }
        if (ilu_valid) {
            uint32_t r = (mac != MAC_NOP) ? 1 : out_r; /* paired ILU writes R1 */
            if (ilu_mask) {
                if (r == 12) write_masked(&s->o[0], &ir, ilu_mask);
                else if (r < 12) write_masked(&s->r[r], &ir, ilu_mask);
            }
            if (o_mux == 1 && o_mask) {
                if (orb == 1) {
                    uint32_t output = o_addr & 0xFu;
                    write_masked(&s->o[output], &ir, o_mask);
                    s->written |= 1u << output;
                } else if (context_writes) {
                    uint32_t constant = vsh_const_index((uint8_t)o_addr);
                    if (constant < CONST_ROWS)
                        write_masked((Vec4 *)K.constants[constant], &ir, o_mask);
                }
            }
        }

        if (d->fin) break; /* FINAL */
    }
}

static void vsh_run(const Vec4 *v, VshState *s)
{
    vsh_run_from(K.program_start, v, s, 0);
}

/* ------------------------------------------------------------------ */
/* Vertex fetch                                                        */
/* ------------------------------------------------------------------ */
static uint32_t attr_type(int i) { return K.attr_format[i] & 0xF; }
static uint32_t attr_size(int i) { return (K.attr_format[i] >> 4) & 0xF; }
static uint32_t attr_stride(int i) { return K.attr_format[i] >> 8; }

static void decode_attr(int i, const uint8_t *p, float out[4])
{
    uint32_t size = attr_size(i);
    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    switch (attr_type(i)) {
    case 0: /* UB_D3D: stored B,G,R,A -> RGBA */
        if (size >= 4 || size == 3) {
            out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f;
            out[3] = size >= 4 ? p[3] / 255.0f : 1.0f;
        } else {
            for (uint32_t k = 0; k < size; ++k) out[k] = p[k] / 255.0f;
        }
        break;
    case 4: /* UB_OGL */
        for (uint32_t k = 0; k < size && k < 4; ++k) out[k] = p[k] / 255.0f;
        break;
    case 1: /* S1: signed normalized */
        for (uint32_t k = 0; k < size && k < 4; ++k) {
            int16_t s; memcpy(&s, p + 2 * k, 2);
            out[k] = (2.0f * s + 1.0f) / 65535.0f;
        }
        break;
    case 5: /* S32K */
        for (uint32_t k = 0; k < size && k < 4; ++k) {
            int16_t s; memcpy(&s, p + 2 * k, 2);
            out[k] = (float)s;
        }
        break;
    case 2: /* float */
        for (uint32_t k = 0; k < size && k < 4; ++k) memcpy(&out[k], p + 4 * k, 4);
        break;
    case 6: { /* CMP: 11:11:10 signed normalized */
        uint32_t w; memcpy(&w, p, 4);
        int32_t x = (int32_t)(w << 21) >> 21, y = (int32_t)(w << 10) >> 21, z = (int32_t)w >> 22;
        out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
        break;
    }
    default: break;
    }
}

static uint32_t attr_inline_dwords(int i)
{
    uint32_t size = attr_size(i);
    if (!size) return 0;
    switch (attr_type(i)) {
    case 0: case 4: return 1;
    case 1: case 5: return (2 * size + 3) / 4;
    case 2: return size;
    case 6: return 1;
    default: return size;
    }
}

static uint32_t inline_vertex_dwords(void)
{
    uint32_t n = 0;
    for (int i = 0; i < 16; ++i) n += attr_inline_dwords(i);
    return n;
}

static void fetch_inline(uint32_t index, Vec4 v[16])
{
    uint32_t stride = inline_vertex_dwords();
    const uint32_t *base = K.inline_data + (size_t)index * stride;
    for (int i = 0; i < 16; ++i) {
        uint32_t n = attr_inline_dwords(i);
        if (n) { decode_attr(i, (const uint8_t *)base, v[i].v); base += n; }
        else memcpy(v[i].v, K.attr_const[i], sizeof(v[i].v));
    }
}

static void fetch_array(uint32_t index, Vec4 v[16])
{
    for (int i = 0; i < 16; ++i) {
        if (attr_size(i)) {
            const uint8_t *p = guest_ptr(K.attr_offset[i] + index * attr_stride(i));
            decode_attr(i, p, v[i].v);
        } else {
            memcpy(v[i].v, K.attr_const[i], sizeof(v[i].v));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t key_offset, key_format, key_w, key_h, key_pitch, key_palette;
    uint64_t hash;
    uint32_t last_frame, checked_frame;
    IDirect3DTexture8 *texture;
} TexCacheEntry;

#define TEX_CACHE_SIZE 1024
static TexCacheEntry g_tex_cache[TEX_CACHE_SIZE];

static int tex_is_linear(uint32_t color)
{
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x17: case 0x1B: case 0x1C:
    case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x2E:
    case 0x2F: case 0x30: case 0x31: case 0x35: case 0x3F: case 0x40: case 0x41:
        return 1;
    default:
        return 0;
    }
}

static int tex_is_dxt(uint32_t color) { return color == 0x0C || color == 0x0E || color == 0x0F; }

/* Bytes per pixel for uncompressed formats, 0 if unsupported. */
static uint32_t tex_bpp(uint32_t color)
{
    switch (color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x17:
    case 0x1A: case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25: case 0x27:
    case 0x28: case 0x29: case 0x35:
        return 2;
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x3A: case 0x3B: case 0x3C:
    case 0x3F: case 0x40: case 0x41:
        return 4;
    default:
        return 0;
    }
}

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t expand4(uint32_t v) { return (v << 4) | v; }

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static uint32_t yuv_to_argb(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    return argb(255, clamp8(r), clamp8(g), clamp8(b));
}

static uint32_t decode_texel(uint32_t color, const uint8_t *p, const uint32_t *palette)
{
    uint16_t w;
    switch (color) {
    case 0x00: case 0x13: return argb(255, p[0], p[0], p[0]);                 /* Y8 */
    case 0x01: case 0x1B: return argb(p[0], p[0], p[0], p[0]);                /* AY8 */
    case 0x19: case 0x1F: return argb(p[0], 255, 255, 255);                   /* A8 */
    case 0x0B: return palette ? palette[p[0]] : argb(255, p[0], p[0], p[0]);  /* I8 */
    case 0x1A: case 0x20: return argb(p[1], p[0], p[0], p[0]);                /* A8Y8 */
    case 0x02: case 0x10: memcpy(&w, p, 2);                                   /* A1R5G5B5 */
        return argb((w & 0x8000) ? 255 : 0, expand5((w >> 10) & 31), expand5((w >> 5) & 31), expand5(w & 31));
    case 0x03: case 0x1C: memcpy(&w, p, 2);                                   /* X1R5G5B5 */
        return argb(255, expand5((w >> 10) & 31), expand5((w >> 5) & 31), expand5(w & 31));
    case 0x04: case 0x1D: memcpy(&w, p, 2);                                   /* A4R4G4B4 */
        return argb(expand4(w >> 12), expand4((w >> 8) & 15), expand4((w >> 4) & 15), expand4(w & 15));
    case 0x05: case 0x11: memcpy(&w, p, 2);                                   /* R5G6B5 */
        return argb(255, expand5(w >> 11), expand6((w >> 5) & 63), expand5(w & 31));
    case 0x27: memcpy(&w, p, 2);                                              /* R6G5B5 */
        return argb(255, expand6(w >> 10), expand5((w >> 5) & 31), expand5(w & 31));
    case 0x28: case 0x17: return argb(255, 0, p[1], p[0]);                    /* G8B8 */
    case 0x29: return argb(255, p[1], 0, p[0]);                               /* R8B8 */
    case 0x35: memcpy(&w, p, 2); return argb(255, w >> 8, w >> 8, w >> 8);    /* Y16 */
    case 0x06: case 0x12: return argb(p[3], p[2], p[1], p[0]);                /* A8R8G8B8 */
    case 0x07: case 0x1E: return argb(255, p[2], p[1], p[0]);                 /* X8R8G8B8 */
    case 0x3A: case 0x3F: return argb(p[3], p[0], p[1], p[2]);                /* A8B8G8R8 */
    case 0x3B: case 0x40: return argb(p[0], p[1], p[2], p[3]);                /* B8G8R8A8 */
    case 0x3C: case 0x41: return argb(p[0], p[3], p[2], p[1]);                /* R8G8B8A8 */
    default: return argb(255, 255, 0, 255);
    }
}

static void swizzle_masks(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my)
{
    uint32_t x = 0, y = 0, bit = 1, mb = 1;
    int done;
    do {
        done = 1;
        if (bit < w) { x |= mb; mb <<= 1; done = 0; }
        if (bit < h) { y |= mb; mb <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mx = x; *my = y;
}

static uint32_t spread(uint32_t v, uint32_t mask)
{
    uint32_t r = 0, bit = 1;
    for (uint32_t m = mask; m; m &= m - 1) {
        uint32_t low = m & (~m + 1);
        if (v & bit) r |= low;
        bit <<= 1;
    }
    return r;
}

static void decode_dxt_block(uint32_t color, const uint8_t *blk, uint32_t out[16])
{
    const uint8_t *cb = color == 0x0C ? blk : blk + 8;
    uint16_t c0, c1; memcpy(&c0, cb, 2); memcpy(&c1, cb + 2, 2);
    uint32_t pal[4];
    uint32_t r0 = expand5(c0 >> 11), g0 = expand6((c0 >> 5) & 63), b0 = expand5(c0 & 31);
    uint32_t r1 = expand5(c1 >> 11), g1 = expand6((c1 >> 5) & 63), b1 = expand5(c1 & 31);
    pal[0] = argb(255, r0, g0, b0);
    pal[1] = argb(255, r1, g1, b1);
    if (c0 > c1 || color != 0x0C) {
        pal[2] = argb(255, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
        pal[3] = argb(255, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
    } else {
        pal[2] = argb(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        pal[3] = 0;
    }
    uint32_t bits; memcpy(&bits, cb + 4, 4);
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (2 * i)) & 3];

    if (color == 0x0E) {           /* DXT2/3 explicit alpha */
        for (int i = 0; i < 16; ++i) {
            uint32_t a = (blk[i / 2] >> ((i & 1) * 4)) & 15;
            out[i] = (out[i] & 0x00FFFFFFu) | (expand4(a) << 24);
        }
    } else if (color == 0x0F) {    /* DXT4/5 interpolated alpha */
        uint32_t a[8]; a[0] = blk[0]; a[1] = blk[1];
        if (a[0] > a[1]) for (int i = 2; i < 8; ++i) a[i] = ((8 - i) * a[0] + (i - 1) * a[1]) / 7;
        else { for (int i = 2; i < 6; ++i) a[i] = ((6 - i) * a[0] + (i - 1) * a[1]) / 5; a[6] = 0; a[7] = 255; }
        uint64_t ab = 0; for (int i = 0; i < 6; ++i) ab |= (uint64_t)blk[2 + i] << (8 * i);
        for (int i = 0; i < 16; ++i)
            out[i] = (out[i] & 0x00FFFFFFu) | (a[(ab >> (3 * i)) & 7] << 24);
    }
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    const uint64_t *q = (const uint64_t *)p;
    size_t words = n / 8;
    for (size_t i = 0; i < words; ++i) { h ^= q[i]; h *= 1099511628211ull; }
    for (size_t i = words * 8; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void tex_geometry(const TexStage *t, uint32_t *w, uint32_t *h, uint32_t *pitch, size_t *bytes)
{
    uint32_t color = (t->format >> 8) & 0xFF;
    if (tex_is_linear(color)) {
        *w = t->image_rect >> 16;
        *h = t->image_rect & 0xFFFF;
        *pitch = t->control1 >> 16;
        *bytes = (size_t)*pitch * *h;
    } else {
        *w = 1u << ((t->format >> 20) & 0xF);
        *h = 1u << ((t->format >> 24) & 0xF);
        if (tex_is_dxt(color)) {
            uint32_t bw = (*w + 3) / 4, bh = (*h + 3) / 4;
            *pitch = bw * (color == 0x0C ? 8 : 16);
            *bytes = (size_t)*pitch * bh;
        } else {
            *pitch = *w * tex_bpp(color);
            *bytes = (size_t)*pitch * *h;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Render targets                                                      */
/* ------------------------------------------------------------------ */
/* SET_SURFACE_COLOR_OFFSET picks the colour surface. Surfaces current when
 * a frame ends are display buffers (front/back, alternating); any other
 * surface is an offscreen target the title will sample later as a texture
 * (bloom, the menu's render-to-texture passes), rendered via d3d8_rt_*. */
extern void d3d8_rt_select(uint32_t key, UINT w, UINT h);
extern IDirect3DTexture8 *d3d8_rt_texture(uint32_t key);
extern void d3d8_rt_present(uint32_t key);
#define M_SURFACE_COLOR_OFFSET 0x0210
static uint32_t g_surf_color;          /* physical address, current */
static uint32_t g_display[2];          /* recent display surfaces */
static uint32_t g_target_key;          /* 0 = back buffer */

static int surf_is_display(uint32_t a)
{
    return !g_display[0] || a == g_display[0] || a == g_display[1];
}

static void select_target(void)
{
    uint32_t key = kenv("KELVIN_ALL_RT") ? g_surf_color
        : ((g_surf_color && !surf_is_display(g_surf_color)) ? g_surf_color : 0);
    if (kenv("KELVIN_NO_RT")) key = 0;
    if (key == g_target_key) return;
    {
        static int n;
        if (n++ < 40)
            fprintf(stderr, "[RT] frame=%u surface=%08X -> %s (display %08X/%08X) clip=%ux%u\n",
                    K.frame, g_surf_color, key ? "offscreen" : "backbuffer",
                    g_display[0], g_display[1], K.clip_h >> 16, K.clip_v >> 16);
    }
    g_target_key = key;
    d3d8_rt_select(key, K.clip_h >> 16, K.clip_v >> 16);
}

/* Decode one w x h image at src into A8R8G8B8 rows (dst, dst_pitch bytes). */
static void decode_image(uint32_t color, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                         uint32_t palette_addr, const uint32_t *palette, void *dst, size_t dst_pitch)
{

    if (tex_is_dxt(color)) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = color == 0x0C ? 8 : 16;
        uint32_t block[16];
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                decode_dxt_block(color, src + (by * bw + bx) * bsize, block);
                for (uint32_t py = 0; py < 4 && by * 4 + py < h; ++py) {
                    uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)(by * 4 + py) * dst_pitch);
                    for (uint32_t px = 0; px < 4 && bx * 4 + px < w; ++px)
                        row[bx * 4 + px] = block[py * 4 + px];
                }
            }
    } else if (color == 0x24 || color == 0x25) {
        /* YUY2 (CR8YB8CB8YA8) / UYVY (YB8CR8YA8CB8), two pixels per dword */
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
            for (uint32_t x = 0; x + 1 < w; x += 2, s += 4) {
                int y0, y1, u, v;
                if (color == 0x24) { y0 = s[0]; u = s[1]; y1 = s[2]; v = s[3]; }
                else { u = s[0]; y0 = s[1]; v = s[2]; y1 = s[3]; }
                row[x] = yuv_to_argb(y0, u, v);
                row[x + 1] = yuv_to_argb(y1, u, v);
            }
        }
    } else if (tex_is_linear(color)) {
        uint32_t bpp = tex_bpp(color);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
            for (uint32_t x = 0; x < w; ++x) row[x] = decode_texel(color, s + x * bpp, palette_addr ? palette : NULL);
        }
    } else {
        uint32_t bpp = tex_bpp(color), mx, my;
        swizzle_masks(w, h, &mx, &my);
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = spread(y, my);
            uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)y * dst_pitch);
            for (uint32_t x = 0; x < w; ++x)
                row[x] = decode_texel(color, src + (size_t)(spread(x, mx) | sy) * bpp,
                                      palette_addr ? palette : NULL);
        }
    }
}

/* Cube textures (SET_TEXTURE_FORMAT bit 2): six faces stored back to back,
 * each a full mip chain padded to 128 bytes (xemu NV2A_CUBEFACE_ALIGNMENT),
 * order +X -X +Y -Y +Z -Z as in D3D11. Only level 0 is uploaded; contents
 * are re-hashed at most once per frame per stage. */
static void bind_cube(int stage)
{
    const TexStage *t = &K.tex[stage];
    uint32_t color = (t->format >> 8) & 0xFF;
    uint32_t w, h, pitch, levels;
    size_t bytes, face = 0;
    static struct { uint32_t offset, format, frame; uint64_t key; } seen[4];
    if (!(t->control0 & (1u << 30)) || !t->offset || !(t->format & 4u) ||
        tex_is_linear(color) || (!tex_is_dxt(color) && !tex_bpp(color))) {
        d3d8_nv2a_psh_set_cube(stage, NULL, 0, 0);
        return;
    }
    tex_geometry(t, &w, &h, &pitch, &bytes);
    if (w != h || !w || w > 2048) { d3d8_nv2a_psh_set_cube(stage, NULL, 0, 0); return; }
    if (seen[stage].key && seen[stage].offset == t->offset && seen[stage].format == t->format &&
        seen[stage].frame == K.frame) {
        d3d8_nv2a_psh_set_cube(stage, NULL, 0, seen[stage].key);
        return;
    }
    levels = (t->format >> 16) & 0xF;
    if (!levels) levels = 1;
    for (uint32_t l = 0; l < levels; ++l) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        if (tex_is_dxt(color)) face += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * (color == 0x0C ? 8 : 16);
        else face += (size_t)lw * lh * tex_bpp(color);
    }
    face = (face + 127) & ~(size_t)127;
    {
        const uint8_t *src = guest_ptr(t->offset);
        uint32_t palette_addr = color == 0x0B ? (t->palette & ~0x3Fu) : 0;
        uint32_t palette[256];
        uint64_t key = (hash_bytes(src, face * 6) ^ ((uint64_t)t->format << 32) ^ t->offset) | 1u;
        uint32_t *pix;
        seen[stage].offset = t->offset; seen[stage].format = t->format;
        seen[stage].frame = K.frame; seen[stage].key = key;
        if (d3d8_nv2a_psh_set_cube(stage, NULL, 0, key)) return;    /* cached */
        pix = (uint32_t *)malloc((size_t)w * h * 4 * 6);
        if (!pix) return;
        if (palette_addr) memcpy(palette, guest_ptr(palette_addr), sizeof(palette));
        for (int f = 0; f < 6; ++f)
            decode_image(color, src + face * f, w, h, pitch, palette_addr, palette,
                         pix + (size_t)f * w * h, (size_t)w * 4);
        if (kenv("KELVIN_DUMP_CUBE") &&
            (!strcmp(kenv("KELVIN_DUMP_CUBE"), "all") ||
             strtoul(kenv("KELVIN_DUMP_CUBE"), NULL, 0) == t->offset)) {
            static uint32_t dumped_frame = ~0u;
            static uint32_t dumped_offset;
            if (dumped_frame != K.frame || dumped_offset != t->offset) {
                dumped_frame = K.frame;
                dumped_offset = t->offset;
                for (int cf = 0; cf < 6; ++cf) {
                    size_t raw_nonzero = 0, decoded_nonblack = 0;
                    const uint8_t *raw = src + face * cf;
                    const uint32_t *decoded = pix + (size_t)cf * w * h;
                    char path[112];
                    FILE *df;
                    for (size_t i = 0; i < face; ++i) raw_nonzero += raw[i] != 0;
                    for (size_t i = 0; i < (size_t)w * h; ++i)
                        decoded_nonblack += (decoded[i] & 0x00FFFFFFu) != 0;
                    fprintf(stderr,
                            "[CUBE] frame=%u stage=%d off=%08X fmt=%08X face=%d "
                            "stride=%zu raw_nonzero=%zu decoded_nonblack=%zu/%zu\n",
                            K.frame, stage, t->offset, t->format, cf, face,
                            raw_nonzero, decoded_nonblack, (size_t)w * h);
                    sprintf(path, "build-release\\cube_%04u_%08X_f%d_%ux%u.bmp",
                            K.frame, t->offset, cf, w, h);
                    df = fopen(path, "wb");
                    if (df) {
                        BITMAPFILEHEADER fh = {0};
                        BITMAPINFOHEADER ih = {0};
                        fh.bfType = 0x4D42;
                        fh.bfOffBits = sizeof(fh) + sizeof(ih);
                        fh.bfSize = fh.bfOffBits + w * h * 4;
                        ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h;
                        ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                        fwrite(&fh, sizeof(fh), 1, df);
                        fwrite(&ih, sizeof(ih), 1, df);
                        fwrite(decoded, (size_t)w * h * 4, 1, df);
                        fclose(df);
                    }
                }
            }
        }
        d3d8_nv2a_psh_set_cube(stage, pix, w, key);
        free(pix);
    }
}

static IDirect3DTexture8 *get_texture(int stage)
{
    const TexStage *t = &K.tex[stage];
    uint32_t color = (t->format >> 8) & 0xFF;
    uint32_t w, h, pitch;
    size_t bytes;
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();

    if (!(t->control0 & (1u << 30)) || !t->offset || !dev)
        return NULL;
    {
        IDirect3DTexture8 *rt = d3d8_rt_texture(t->offset & 0x03FFFFFFu);
        if (rt) return rt;
    }
    if (!tex_is_dxt(color) && !tex_bpp(color)) {
        K.stats.unsupported_texture_formats++;
        return NULL;
    }
    tex_geometry(t, &w, &h, &pitch, &bytes);
    if (color == 0x24 || color == 0x25) {
        static uint32_t last_yuv_frame = ~0u;
        if (last_yuv_frame != K.frame && K.frame < 20) {
            last_yuv_frame = K.frame;
            fprintf(stderr,
                    "[TEX-YUV] frame=%u stage=%d off=%08X fmt=%08X ctl0=%08X ctl1=%08X "
                    "rect=%08X filter=%08X address=%08X -> %ux%u pitch=%u bytes=%zu\n",
                    K.frame, stage, t->offset, t->format, t->control0, t->control1,
                    t->image_rect, t->filter, t->address, w, h, pitch, bytes);
        }
    }
    if (!w || !h || w > 4096 || h > 4096 || bytes > 64u * 1024 * 1024)
        return NULL;

    const uint8_t *src = guest_ptr(t->offset);
    uint32_t palette_addr = color == 0x0B ? (t->palette & ~0x3Fu) : 0;

    TexCacheEntry *slot = NULL, *victim = &g_tex_cache[0];
    for (int i = 0; i < TEX_CACHE_SIZE; ++i) {
        TexCacheEntry *e = &g_tex_cache[i];
        if (e->texture && e->key_offset == t->offset && e->key_format == t->format &&
            e->key_w == w && e->key_h == h && e->key_pitch == pitch &&
            e->key_palette == palette_addr) { slot = e; break; }
        if (!e->texture || e->last_frame < victim->last_frame) victim = e;
    }
    uint64_t hash;
    if (slot && slot->checked_frame == K.frame) {
        slot->last_frame = K.frame;
        return slot->texture;
    }
    hash = hash_bytes(src, bytes) ^ (palette_addr ? hash_bytes(guest_ptr(palette_addr), 1024) : 0);
    if (slot && slot->hash == hash) {
        slot->checked_frame = slot->last_frame = K.frame;
        return slot->texture;
    }
    if (!slot) {
        /* Never release a texture already handed out this frame: submit()
         * still holds the pointers for its other stages, and earlier draws'
         * wrappers are still referenced (a demo scene with >256 textures
         * evicted one mid-draw and SetTexture read freed memory). */
        if (victim->texture && victim->last_frame == K.frame) {
            static int warned;
            if (!warned++) fprintf(stderr, "[KELVIN] texture cache full this frame (%d)\n", TEX_CACHE_SIZE);
            return NULL;
        }
        slot = victim;
        if (slot->texture) { slot->texture->lpVtbl->Release(slot->texture); slot->texture = NULL; }
        if (dev->lpVtbl->CreateTexture(dev, w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED,
                                       &slot->texture) != 0 || !slot->texture) {
            slot->texture = NULL;
            return NULL;
        }
        slot->key_offset = t->offset; slot->key_format = t->format;
        slot->key_w = w; slot->key_h = h; slot->key_pitch = pitch; slot->key_palette = palette_addr;
    }

    D3DLOCKED_RECT lr;
    if (slot->texture->lpVtbl->LockRect(slot->texture, 0, &lr, NULL, 0) != 0 || !lr.pBits)
        return NULL;

    uint32_t palette[256];
    if (palette_addr) memcpy(palette, guest_ptr(palette_addr), sizeof(palette));
    decode_image(color, src, w, h, pitch, palette_addr, palette, lr.pBits, (size_t)lr.Pitch);
    {
        /* KELVIN_DUMP_TEX=<frame>: write each texture decoded in that frame. */
        static long dump_frame = -2;
        if (dump_frame == -2) {
            const char *e = kenv("KELVIN_DUMP_TEX");
            dump_frame = e ? strtol(e, NULL, 0) : -1;
        }
        static int dumped_any;
        /* KELVIN_DUMP_TEX=all dumps the first 64 decodes of any frame. */
        if (dump_frame == -2 + 0 && 0) dump_frame = -1;
        if ((long)K.frame == dump_frame ||
            (kenv("KELVIN_DUMP_TEX") && !strcmp(kenv("KELVIN_DUMP_TEX"), "all") &&
             dumped_any++ < 64)) {
            char path[96];
            FILE *f;
            sprintf(path, "build-release\\tex_%04u_%08X_%02X_%ux%u.bmp", K.frame,
                    t->offset, color, w, h);
            f = fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER fh = {0};
                BITMAPINFOHEADER ih = {0};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + w * h * 4;
                ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h;
                ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (uint32_t y = 0; y < h; ++y)
                    fwrite((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, w * 4, 1, f);
                fclose(f);
                fprintf(stderr, "[KELVIN] dumped %s\n", path);
            }
        }
    }
    slot->texture->lpVtbl->UnlockRect(slot->texture, 0);
    slot->hash = hash;
    slot->checked_frame = slot->last_frame = K.frame;
    K.stats.textures_decoded++;
    return slot->texture;
}

/* ------------------------------------------------------------------ */
/* Raster state                                                        */
/* ------------------------------------------------------------------ */
static DWORD map_blend(uint32_t f)
{
    switch (f) {
    case 0x0000: return D3DBLEND_ZERO;
    case 0x0001: return D3DBLEND_ONE;
    case 0x0300: return D3DBLEND_SRCCOLOR;
    case 0x0301: return D3DBLEND_INVSRCCOLOR;
    case 0x0302: return D3DBLEND_SRCALPHA;
    case 0x0303: return D3DBLEND_INVSRCALPHA;
    case 0x0304: return D3DBLEND_DESTALPHA;
    case 0x0305: return D3DBLEND_INVDESTALPHA;
    case 0x0306: return D3DBLEND_DESTCOLOR;
    case 0x0307: return D3DBLEND_INVDESTCOLOR;
    case 0x0308: return D3DBLEND_SRCALPHASAT;
    default: return D3DBLEND_ONE;
    }
}

static DWORD map_cmp(uint32_t f) { return (f >= 0x200 && f <= 0x207) ? (f - 0x200 + 1) : D3DCMP_ALWAYS; }

static void apply_state(IDirect3DDevice8 *dev, int textured[4])
{
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, K.depth_test ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, K.depth_mask ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, map_cmp(K.depth_func));
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, K.blend_enable ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, map_blend(K.sfactor));
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, map_blend(K.dfactor));
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, K.alpha_test ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAFUNC, map_cmp(K.alpha_func));
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAREF, K.alpha_ref & 0xFF);
    {
        DWORD mask = 0;
        if (K.color_mask & 0x00010000u) mask |= 1; /* red */
        if (K.color_mask & 0x00000100u) mask |= 2; /* green */
        if (K.color_mask & 0x00000001u) mask |= 4; /* blue */
        if (K.color_mask & 0x01000000u) mask |= 8; /* alpha */
        dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, mask);
    }
    if (kenv("KELVIN_SIMPLE")) {
        /* bisection aid: 1 = no depth, 2 = no blending, 4 = no alpha test */
        long bits = strtol(kenv("KELVIN_SIMPLE"), NULL, 0);
        if (bits & 1) dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        if (bits & 2) dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        if (bits & 4) dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    }

    int last = -1;
    for (int s = 0; s < 4; ++s) {
        if (!textured[s]) break;
        last = s;
    }
    for (int s = 0; s < 4; ++s) {
        if (s <= last) {
            DWORD addr_u = (K.tex[s].address & 0xF) == 1 ? D3DTADDRESS_WRAP :
                           (K.tex[s].address & 0xF) == 2 ? D3DTADDRESS_MIRROR : D3DTADDRESS_CLAMP;
            DWORD addr_v = ((K.tex[s].address >> 8) & 0xF) == 1 ? D3DTADDRESS_WRAP :
                           ((K.tex[s].address >> 8) & 0xF) == 2 ? D3DTADDRESS_MIRROR : D3DTADDRESS_CLAMP;
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLOROP, D3DTOP_MODULATE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLORARG2, s ? D3DTA_CURRENT : D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAARG2, s ? D3DTA_CURRENT : D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_TEXCOORDINDEX, s);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ADDRESSU, addr_u);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ADDRESSV, addr_v);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        } else if (s == 0) {
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        } else {
            dev->lpVtbl->SetTexture(dev, s, NULL);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->lpVtbl->SetTextureStageState(dev, s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */
/* Set by the title for frames it wants every draw logged ([DRAW] lines). */
int g_kelvin_log_draws;

static uint32_t pack_color(const float c[4])
{
    int r = (int)(c[0] * 255.0f + 0.5f), g = (int)(c[1] * 255.0f + 0.5f);
    int b = (int)(c[2] * 255.0f + 0.5f), a = (int)(c[3] * 255.0f + 0.5f);
    return argb(clamp8(a), clamp8(r), clamp8(g), clamp8(b));
}

static float nan_to_one(float f) { return f != f ? 1.0f : f; }

static void transform_vertex(const Vec4 v[16], OutVertex *o, const float tex_scale[4][2])
{
    Vec4 pos, d0, d1, t[4];
    if (K.program_mode) {
        VshState s;
        vsh_run(v, &s);
        pos = s.o[0]; d0 = s.o[3]; d1 = s.o[4];
        /* Without combiner emulation a program that never writes oD0 would
         * modulate every texture by black; treat the colour as unused. */
        if (!(s.written & (1u << 3))) d0.v[0] = d0.v[1] = d0.v[2] = d0.v[3] = 1.0f;
        for (int i = 0; i < 4; ++i) t[i] = s.o[9 + i];
        /* oPos.xyz is already screen space divided by w (D3D's program
         * epilogue does the rcc/mad); w only carries 1/w for clipping and
         * perspective interpolation. xemu re-multiplies by w to build clip
         * coordinates; the D3D11 layer does the same from rhw. Vertices
         * behind the eye get huge x/y here and are clipped there. */
        o->x = pos.v[0];
        o->y = pos.v[1];
        o->z = K.clip_max != 0.0f ? pos.v[2] / K.clip_max : pos.v[2];
    } else {
        const Vec4 *p = &v[0];
        for (int r = 0; r < 4; ++r)
            pos.v[r] = p->v[0] * K.composite[r][0] + p->v[1] * K.composite[r][1] +
                       p->v[2] * K.composite[r][2] + p->v[3] * K.composite[r][3];
        float w = pos.v[3] == 0.0f ? 1.0f : pos.v[3];
        o->x = pos.v[0] / w + K.viewport_offset[0];
        o->y = pos.v[1] / w + K.viewport_offset[1];
        o->z = K.clip_max != 0.0f ? (pos.v[2] / w) / K.clip_max : pos.v[2] / w;
        d0 = v[3]; d1 = v[4];
        for (int i = 0; i < 4; ++i) t[i] = v[9 + i];
    }
    /* XDK vertex-program epilogues have already divided oPos.xyz and store
     * reciprocal clip-W in oPos.w.  Fixed-function transforms still carry
     * ordinary clip-W.  XYZRHW therefore takes program oPos.w directly but
     * the fixed-function path must invert it.  Inverting both paths turned
     * reciprocal-W back into W, so the host shader reconstructed the wrong
     * homogeneous position and near-plane triangles on the close-up menu
     * model stretched across the screen. */
    float w = pos.v[3];
    if (w != w) w = 1.0f;
    if (w > 0.0f || w == 0.0f && !signbit(w)) w = w < 5.421011e-20f ? 5.421011e-20f : (w > 1.8446744e19f ? 1.8446744e19f : w);
    else w = w > -5.421011e-20f ? -5.421011e-20f : (w < -1.8446744e19f ? -1.8446744e19f : w);
    o->rhw = K.program_mode ? w : 1.0f / w;
    /* no z clamp: the D3D11 layer rebuilds clip space from (x, y, z, rhw)
     * and lets the rasteriser clip; clamping would fold geometry behind the
     * eye onto the near plane. */
    if (o->z != o->z) o->z = 0.0f;
    for (int i = 0; i < 4; ++i) { d0.v[i] = nan_to_one(d0.v[i]); d1.v[i] = nan_to_one(d1.v[i]); }
    o->diffuse = pack_color(d0.v);
    o->specular = pack_color(d1.v);
    for (int i = 0; i < 4; ++i) {
        float q = t[i].v[3];
        float u = t[i].v[0], vv = t[i].v[1];
        float r = t[i].v[2];
        if (q != 0.0f && q != 1.0f && q == q) { u /= q; vv /= q; r /= q; }
        o->tex[i][0] = u * tex_scale[i][0];
        o->tex[i][1] = vv * tex_scale[i][1];
        o->tex[i][2] = r;
        o->tex[i][3] = 1.0f;
    }
}

static OutVertex *g_verts;
static uint32_t g_verts_cap;

static OutVertex *reserve_verts(uint32_t n)
{
    if (n > g_verts_cap) {
        free(g_verts);
        g_verts_cap = n + 1024;
        g_verts = (OutVertex *)malloc((size_t)g_verts_cap * sizeof(OutVertex));
        if (!g_verts) g_verts_cap = 0;
    }
    return g_verts;
}

/* Vertex transform worker pool. Large draws (the skinned Godzilla mesh is
 * 4602 vertices, drawn in several passes) dominate the translator; fetch and
 * transform only read translator state, so chunks run on worker threads.
 * KELVIN_THREADS=<n> overrides the worker count (0 = serial). */
#define XF_CHUNK 128
static struct {
    HANDLE start_sem, done_evt;
    volatile LONG next, pending;
    int nthreads, inited;
    int (*fetch)(uint32_t i, Vec4 v[16]);
    OutVertex *src;
    const float (*ts)[2];
    uint32_t count, chunk;
} XF;

static void xf_work(void)
{
    Vec4 v[16];
    for (;;) {
        LONG c = InterlockedIncrement(&XF.next) - 1;
        uint32_t b = (uint32_t)c * XF.chunk, e;
        if (b >= XF.count) break;
        e = b + XF.chunk < XF.count ? b + XF.chunk : XF.count;
        for (uint32_t i = b; i < e; ++i) {
            if (!XF.fetch(i, v)) memset(v, 0, sizeof(v));
            transform_vertex(v, &XF.src[i], XF.ts);
        }
    }
}

static DWORD WINAPI xf_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(XF.start_sem, INFINITE);
        xf_work();
        if (InterlockedDecrement(&XF.pending) == 0) SetEvent(XF.done_evt);
    }
}

static int xf_parallel(uint32_t count, int (*fetch)(uint32_t, Vec4 *), OutVertex *src,
                       const float ts[4][2])
{
    if (!XF.inited) {
        SYSTEM_INFO si;
        const char *e = kenv("KELVIN_THREADS");
        GetSystemInfo(&si);
        XF.nthreads = e ? atoi(e) : (int)si.dwNumberOfProcessors - 1;
        if (XF.nthreads > 8) XF.nthreads = 8;
        if (XF.nthreads < 0) XF.nthreads = 0;
        XF.start_sem = CreateSemaphoreA(NULL, 0, 64, NULL);
        XF.done_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
        for (int i = 0; i < XF.nthreads; ++i) CreateThread(NULL, 0, xf_thread, NULL, 0, NULL);
        XF.inited = 1;
    }
    if (XF.nthreads <= 0) return 0;
    /* decode the program on this thread: the decoder caches lazily */
    if (K.program_mode)
        for (uint32_t slot = K.program_start; slot < PROGRAM_SLOTS; ++slot)
            if (vdec(slot)->fin) break;
    /* Demo gameplay is ~1000 draws of under 512 vertices: split every
     * draw worth a thread wake-up, and wake only as many workers as there
     * are chunks beyond the one this thread takes. */
    {
        uint32_t chunk = count / (uint32_t)(XF.nthreads + 1);
        int wake;
        if (chunk < 32) chunk = 32;
        if (chunk > XF_CHUNK) chunk = XF_CHUNK;
        wake = (int)((count + chunk - 1) / chunk) - 1;
        if (wake > XF.nthreads) wake = XF.nthreads;
        if (wake <= 0) return 0;
        XF.fetch = fetch; XF.src = src; XF.ts = ts; XF.count = count; XF.chunk = chunk;
        XF.next = 0;
        XF.pending = wake;
        MemoryBarrier();
        ReleaseSemaphore(XF.start_sem, wake, NULL);
    }
    xf_work();
    WaitForSingleObject(XF.done_evt, INFINITE);
    return 1;
}

/* per-phase translator time (QPC ticks), read by the host's frame report */
long long g_kelvin_perf[4];   /* 0 textures, 1 vertex transform, 2 D3D submit, 3 draws */
static long long kperf_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static uint32_t g_inline_src, g_inline_in_call;
static int fetch_indexed_cb(uint32_t i, Vec4 v[16]);
static void fetch_array(uint32_t index, Vec4 v[16]);

/* Indexed draws name each shared vertex several times (~6 references per
 * vertex in a closed triangle mesh); the transform used to run per index.
 * Run it once per distinct index and copy. */
static uint32_t *g_uniq, g_uniq_cap;
static int32_t *g_umap; static uint32_t g_umap_cap;
static OutVertex *g_utmp; static uint32_t g_utmp_cap;
static int fetch_unique_cb(uint32_t i, Vec4 v[16]) { fetch_array(g_uniq[i], v); return 1; }

static int xf_indexed_unique(uint32_t count, OutVertex *src, float tex_scale[4][2])
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0, range, n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t x = K.indices[i];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    range = hi - lo + 1;
    if (range > 1u << 20 || range > count * 8u + 1024u) return 0;
    if (range > g_umap_cap) {
        int32_t *m = (int32_t *)realloc(g_umap, range * sizeof(int32_t));
        if (!m) return 0;
        g_umap = m; g_umap_cap = range;
    }
    if (count > g_uniq_cap) {
        uint32_t *u = (uint32_t *)realloc(g_uniq, count * sizeof(uint32_t));
        OutVertex *t = (OutVertex *)realloc(g_utmp, count * sizeof(OutVertex));
        if (u) g_uniq = u;
        if (t) g_utmp = t;
        if (!u || !t) return 0;
        g_uniq_cap = g_utmp_cap = count;
    }
    memset(g_umap, 0xFF, range * sizeof(int32_t));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t k = K.indices[i] - lo;
        if (g_umap[k] < 0) { g_umap[k] = (int32_t)n; g_uniq[n++] = K.indices[i]; }
    }
    if (n * 4 > count * 3) return 0;   /* little sharing: plain path is as good */
    if (!(n >= 80 && xf_parallel(n, fetch_unique_cb, g_utmp, (const float (*)[2])tex_scale))) {
        Vec4 v[16];
        for (uint32_t i = 0; i < n; ++i) {
            fetch_array(g_uniq[i], v);
            transform_vertex(v, &g_utmp[i], tex_scale);
        }
    }
    for (uint32_t i = 0; i < count; ++i)
        src[i] = g_utmp[g_umap[K.indices[i] - lo]];
    return 1;
}

static void submit(uint32_t count, int (*fetch)(uint32_t i, Vec4 v[16]))
{
    long long _t0, _t1;
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    if (!dev || count == 0) return;
    if ((K.color_mask & 0x01010101u) == 0 && !K.depth_mask) return;
    {   /* debug: KELVIN_SKIP_TEX=<hex offset>[,...] drops draws sampling those textures */
        const char *sk = kenv("KELVIN_SKIP_TEX");
        if (sk) {
            char buf[256]; char *tok, *ctx = NULL;
            strncpy(buf, sk, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
            for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx))
                if ((uint32_t)strtoul(tok, NULL, 16) == K.tex[0].offset) return;
        }
    }
    if (kenv("KELVIN_ONLY_VP") && (!K.program_mode ||
        (uint32_t)atoi(kenv("KELVIN_ONLY_VP")) != K.program_start))
        return;   /* debug: isolate one vertex program's draws */

    /* Expand quads, quad strips and polygons to triangle lists here; the
     * rest map directly onto D3D primitive types. */
    uint32_t prim = K.prim, out_count = count;
    D3DPRIMITIVETYPE d3dprim;
    switch (prim) {
    case 1: d3dprim = D3DPT_POINTLIST; break;
    case 2: d3dprim = D3DPT_LINELIST; break;
    case 3: case 4: d3dprim = D3DPT_LINESTRIP; break;
    case 5: d3dprim = D3DPT_TRIANGLELIST; break;
    case 6: d3dprim = D3DPT_TRIANGLESTRIP; break;
    case 7: case 10: d3dprim = D3DPT_TRIANGLEFAN; break;
    case 8: d3dprim = D3DPT_TRIANGLELIST; out_count = count / 4 * 6; break;
    case 9: d3dprim = D3DPT_TRIANGLELIST; out_count = count >= 4 ? (count - 2) / 2 * 6 : 0; break;
    default: return;
    }
    if (!out_count) return;

    /* D3D's fixed-function/state vertex shader normally materializes the
     * screen-space transform in c96-c99 before these passthrough programs
     * run.  Some titles submit that state immediately before the retained
     * push-buffer range we first observe, leaving a completely empty matrix.
     * Recover the canonical 640x480 pre-transformed-vertex mapping only for
     * that unmistakable all-zero state; any guest-provided matrix wins. */
    if (K.program_mode && (K.program_start == 0 || K.program_start == 17)) {
        int empty = 1;
        for (int r = 96; r <= 99 && empty; ++r)
            for (int c = 0; c < 4; ++c)
                if (K.constants[r][c] != 0.0f) { empty = 0; break; }
        if (empty) {
            static int reported;
            memset(&K.constants[96][0], 0, 4 * 4 * sizeof(float));
            K.constants[96][0] = 1.0f / 320.0f;
            K.constants[96][3] = -1.0f;
            K.constants[97][1] = -1.0f / 240.0f;
            K.constants[97][3] = 1.0f;
            K.constants[99][3] = 1.0f;
            if (!reported++)
                fprintf(stderr, "[KELVIN] recovered missing 640x480 screen transform in c96-c99\n");
        }
    }

    int textured[4] = {0, 0, 0, 0};
    float tex_scale[4][2];
    IDirect3DTexture8 *tex[4] = {NULL, NULL, NULL, NULL};
    _t0 = kperf_now();
    for (int s = 0; s < 4; ++s) {
        tex_scale[s][0] = tex_scale[s][1] = 1.0f;
        tex[s] = get_texture(s);
        if (tex[s]) {
            uint32_t color = (K.tex[s].format >> 8) & 0xFF;
            textured[s] = 1;
            if (tex_is_linear(color)) {
                /* linear textures are addressed in texels */
                uint32_t w, h, pitch; size_t bytes;
                tex_geometry(&K.tex[s], &w, &h, &pitch, &bytes);
                tex_scale[s][0] = 1.0f / (float)w;
                tex_scale[s][1] = 1.0f / (float)h;
            }
        }
    }

    _t1 = kperf_now(); g_kelvin_perf[0] += _t1 - _t0; _t0 = _t1;
    OutVertex *src = reserve_verts(count + out_count);
    if (!src) return;
    OutVertex *dst = src + count;
    Vec4 v[16];
    if (fetch == fetch_indexed_cb && count >= 16 && !g_kelvin_log_draws && !kenv("KELVIN_DUMP_VP") &&
        xf_indexed_unique(count, src, tex_scale)) {
        /* indexed: each distinct vertex transformed once, then copied per index */
    } else
    if (count >= 80 && !g_kelvin_log_draws && !kenv("KELVIN_DUMP_VP") &&
        xf_parallel(count, fetch, src, (const float (*)[2])tex_scale)) {
        /* transformed by the worker pool */
    } else
    for (uint32_t i = 0; i < count; ++i) {
        if (!fetch(i, v)) memset(v, 0, sizeof(v));
        transform_vertex(v, &src[i], tex_scale);
        if (g_kelvin_log_draws && (src[i].x > 1e4f || src[i].x < -1e4f ||
                                   src[i].y > 1e4f || src[i].y < -1e4f)) {
            /* exploding vertex: show what the program was fed */
            static int shown;
            if (shown++ < 24)
                fprintf(stderr, "[HUGE] draw#%u vtx %u v0=(%g,%g,%g,%g) v3=(%g,%g,%g,%g) fmt0=%08X off0=%08X "
                        "out=(%g,%g,%g rhw=%g) c96=(%g,%g,%g,%g) c99=(%g,%g,%g,%g)\n",
                        K.stats.draws + 1, i, v[0].v[0], v[0].v[1], v[0].v[2], v[0].v[3],
                        v[3].v[0], v[3].v[1], v[3].v[2], v[3].v[3], K.attr_format[0], K.attr_offset[0],
                        src[i].x, src[i].y, src[i].z, src[i].rhw,
                        K.constants[96][0], K.constants[96][1], K.constants[96][2], K.constants[96][3],
                        K.constants[99][0], K.constants[99][1], K.constants[99][2], K.constants[99][3]);
        }
        if (i < 96 && K.program_mode && kenv("KELVIN_DUMP_VP") &&
            (uint32_t)atoi(kenv("KELVIN_DUMP_VP")) == K.program_start) {
            /* one-shot dump of a vertex program, its inputs and outputs */
            static int dumped;
            static uint32_t dump_draw;
            if (dumped && dump_draw == K.stats.draws && i < 96) {
                VshState s;
                vsh_run(v, &s);
                fprintf(stderr, "[VP-VTX] %u v0=(%.3f,%.3f,%.3f) v1=(%g,%g,%g,%g) v2=(%g,%g) o0=(%.1f,%.1f,%.4g,%.4g)\n", i,
                        v[0].v[0], v[0].v[1], v[0].v[2], v[1].v[0], v[1].v[1], v[1].v[2], v[1].v[3],
                        v[2].v[0], v[2].v[1], s.o[0].v[0], s.o[0].v[1], s.o[0].v[2], s.o[0].v[3]);
            }
            if (!dumped++) {
                VshState s;
                dump_draw = K.stats.draws;
                vsh_run(v, &s);
                for (int r = 0; r < CONST_ROWS; ++r)
                    if (K.constants[r][0] != 0.0f || K.constants[r][1] != 0.0f || K.constants[r][2] != 0.0f || K.constants[r][3] != 0.0f)
                        fprintf(stderr, "[VP-CONST] c%d = (%g, %g, %g, %g)\n", r,
                                K.constants[r][0], K.constants[r][1], K.constants[r][2], K.constants[r][3]);
                fprintf(stderr, "[VP-DUMP] start=%u count=%u prim=%u\n", K.program_start, count, K.prim);
                for (int a = 0; a < 16; ++a)
                    fprintf(stderr, "[VP-DUMP] v%d fmt=%08X off=%08X = (%g, %g, %g, %g)\n", a,
                            K.attr_format[a], K.attr_offset[a], v[a].v[0], v[a].v[1], v[a].v[2], v[a].v[3]);
                for (uint32_t sl = K.program_start; sl < PROGRAM_SLOTS; ++sl) {
                    fprintf(stderr, "[VP-DUMP] slot %u: %08X %08X %08X %08X\n", sl, K.program[sl][0],
                            K.program[sl][1], K.program[sl][2], K.program[sl][3]);
                    if (K.program[sl][3] & 1) break;
                }
                for (int o = 0; o < 13; ++o)
                    fprintf(stderr, "[VP-DUMP] o%d = (%g, %g, %g, %g)\n", o,
                            s.o[o].v[0], s.o[o].v[1], s.o[o].v[2], s.o[o].v[3]);
                for (int r = 0; r < 12; ++r)
                    fprintf(stderr, "[VP-DUMP] r%d = (%g, %g, %g, %g)\n", r,
                            s.r[r].v[0], s.r[r].v[1], s.r[r].v[2], s.r[r].v[3]);
            }
        }
        if (i == 0 && K.stats.draws < 3) {
            uint32_t ps = K.program_start;
            fprintf(stderr, "[KELVIN] dbg start=%u load=%u slot=%08X %08X %08X %08X "
                    "v0=(%g,%g,%g,%g) v9=(%g,%g) c96=(%g,%g,%g,%g) fmt0=%08X\n",
                    ps, K.program_load, K.program[ps][0], K.program[ps][1],
                    K.program[ps][2], K.program[ps][3], v[0].v[0], v[0].v[1],
                    v[0].v[2], v[0].v[3], v[9].v[0], v[9].v[1], K.constants[96][0],
                    K.constants[96][1], K.constants[96][2], K.constants[96][3],
                    K.attr_format[0]);
        }
    }
    const OutVertex *draw = src;
    if (prim == 8) {
        for (uint32_t q = 0; q < count / 4; ++q) {
            const OutVertex *a = &src[q * 4];
            dst[q * 6 + 0] = a[0]; dst[q * 6 + 1] = a[1]; dst[q * 6 + 2] = a[2];
            dst[q * 6 + 3] = a[0]; dst[q * 6 + 4] = a[2]; dst[q * 6 + 5] = a[3];
        }
        draw = dst;
    } else if (prim == 9) {
        uint32_t n = 0;
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            dst[n++] = src[i]; dst[n++] = src[i + 1]; dst[n++] = src[i + 3];
            dst[n++] = src[i]; dst[n++] = src[i + 3]; dst[n++] = src[i + 2];
        }
        draw = dst;
    }

    UINT prims;
    switch (d3dprim) {
    case D3DPT_POINTLIST: prims = out_count; break;
    case D3DPT_LINELIST: prims = out_count / 2; break;
    case D3DPT_LINESTRIP: prims = out_count - 1; break;
    case D3DPT_TRIANGLELIST: prims = out_count / 3; break;
    default: prims = out_count >= 3 ? out_count - 2 : 0; break;
    }
    if (!prims) return;

    _t1 = kperf_now(); g_kelvin_perf[1] += _t1 - _t0; _t0 = _t1;
    apply_state(dev, textured);
    if (kenv("KELVIN_NO_PSH")) {
        d3d8_nv2a_psh_set(NULL);
    } else {
        K.psh.alpha_test = K.alpha_test;
        K.psh.alpha_func = K.alpha_func;
        K.psh.alpha_ref = K.alpha_ref;
        K.psh.cube_mask = 0;
        for (int s = 0; s < 4; ++s) {
            uint32_t mode = (K.psh.shader_stage_program >> (5 * s)) & 0x1F;
            if (mode == 0x03 && (K.tex[s].format & 4u)) {
                bind_cube(s);
                K.psh.cube_mask |= 1u << s;
            } else {
                d3d8_nv2a_psh_set_cube(s, NULL, 0, 0);
            }
        }
        d3d8_nv2a_psh_set(&K.psh);
    }
    for (int s = 0; s < 4; ++s)
        if (textured[s]) dev->lpVtbl->SetTexture(dev, s, (IDirect3DBaseTexture8 *)tex[s]);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX4 |
                                  0x00AA0000u /* TEXCOORDSIZE4 x4 */);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->DrawPrimitiveUP(dev, d3dprim, prims, draw, sizeof(OutVertex));
    g_kelvin_perf[2] += kperf_now() - _t0; g_kelvin_perf[3]++;

    K.stats.draws++;
    K.stats.vertices += count;
    if (K.program_mode) K.stats.program_draws++; else K.stats.ff_draws++;

    if (g_kelvin_log_draws) {
        /* per-draw log for scene analysis: screen bbox, depth range, state */
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (uint32_t i = 0; i < count; ++i) {
            if (src[i].x < x0) x0 = src[i].x; if (src[i].x > x1) x1 = src[i].x;
            if (src[i].y < y0) y0 = src[i].y; if (src[i].y > y1) y1 = src[i].y;
            if (src[i].z < z0) z0 = src[i].z; if (src[i].z > z1) z1 = src[i].z;
        }
        fprintf(stderr, "[DRAW] #%u prim=%u n=%u %s vp=%u box=(%.0f..%.0f,%.0f..%.0f) z=%.3f..%.3f "
                "c0=%08X tex=%08X/%08X,%08X/%08X blend=%u %X/%X atest=%u depth=%u/%u cmask=%08X src=%s a0=%08X/%08X\n",
                K.stats.draws, prim, count, K.program_mode ? "P" : "F", K.program_start,
                x0, x1, y0, y1, z0, z1, src[0].diffuse,
                textured[0] ? K.tex[0].offset : 0, textured[0] ? K.tex[0].format : 0,
                textured[1] ? K.tex[1].offset : 0, textured[1] ? K.tex[1].format : 0,
                K.blend_enable, K.sfactor, K.dfactor, K.alpha_test, K.depth_test,
                K.depth_mask, K.color_mask,
                K.inline_count ? (g_inline_in_call ? "inline-call" : "inline") : K.index_count ? "indexed" : "array",
                K.inline_count ? g_inline_src : K.attr_offset[0], K.attr_format[0]);
        fprintf(stderr, "[PSH] vp=%u ctl=%08X stage=%08X other=%08X cube=%X final=%08X/%08X "
                "tex2=%08X/%08X tex3=%08X/%08X "
                "rgb0=%08X/%08X alpha0=%08X/%08X rgb1=%08X/%08X alpha1=%08X/%08X\n",
                K.program_mode ? K.program_start : 0, K.psh.combiner_control,
                K.psh.shader_stage_program, K.psh.other_stage_input, K.psh.cube_mask,
                K.psh.final0, K.psh.final1,
                textured[2] ? K.tex[2].offset : 0, textured[2] ? K.tex[2].format : 0,
                textured[3] ? K.tex[3].offset : 0, textured[3] ? K.tex[3].format : 0,
                K.psh.rgb_in[0], K.psh.rgb_out[0], K.psh.alpha_in[0], K.psh.alpha_out[0],
                K.psh.rgb_in[1], K.psh.rgb_out[1], K.psh.alpha_in[1], K.psh.alpha_out[1]);
        fprintf(stderr, "[PSH-FACTORS] f0=%08X,%08X,%08X,%08X,%08X "
                "f1=%08X,%08X,%08X,%08X,%08X final=%08X/%08X\n",
                K.psh.factor0[0], K.psh.factor0[1], K.psh.factor0[2],
                K.psh.factor0[3], K.psh.factor0[4], K.psh.factor1[0],
                K.psh.factor1[1], K.psh.factor1[2], K.psh.factor1[3],
                K.psh.factor1[4], K.psh.final_factor0, K.psh.final_factor1);
    }

    if (K.stats.draws <= 12) {
        fprintf(stderr, "[KELVIN] draw #%u prim=%u verts=%u %s tex0=%08X fmt=%08X "
                "v0=(%.1f,%.1f,%.3f,%.4f) c=%08X uv=(%.3f,%.3f)\n",
                K.stats.draws, prim, count, K.program_mode ? "program" : "fixed",
                K.tex[0].offset, K.tex[0].format, src[0].x, src[0].y, src[0].z, src[0].rhw,
                src[0].diffuse, src[0].tex[0][0], src[0].tex[0][1]);
        fprintf(stderr, "[KELVIN]   blend=%u %X/%X eq=%X atest=%u func=%X ref=%X "
                "depth=%u func=%X mask=%u cmask=%08X clip=%g\n",
                K.blend_enable, K.sfactor, K.dfactor, K.blend_eq, K.alpha_test,
                K.alpha_func, K.alpha_ref, K.depth_test, K.depth_func, K.depth_mask,
                K.color_mask, K.clip_max);
    }
}

static int fetch_inline_cb(uint32_t i, Vec4 v[16]) { fetch_inline(i, v); return 1; }
static int fetch_indexed_cb(uint32_t i, Vec4 v[16]) { fetch_array(K.indices[i], v); return 1; }
static int fetch_arrays_cb(uint32_t i, Vec4 v[16]) { fetch_array(K.array_start + i, v); return 1; }

static void end_primitive(void)
{
    if (K.inline_count) {
        uint32_t stride = inline_vertex_dwords();
        if (stride) submit(K.inline_count / stride, fetch_inline_cb);
    } else if (K.index_count) {
        submit(K.index_count, fetch_indexed_cb);
    } else if (K.array_count) {
        submit(K.array_count, fetch_arrays_cb);
    }
    K.inline_count = 0;
    K.index_count = 0;
    K.array_count = 0;
    K.have_arrays = 0;
}

static void push_index(uint32_t idx)
{
    if (K.index_count >= K.index_cap) {
        uint32_t cap = K.index_cap ? K.index_cap * 2 : 4096;
        uint32_t *p = (uint32_t *)realloc(K.indices, cap * sizeof(uint32_t));
        if (!p) return;
        K.indices = p; K.index_cap = cap;
    }
    K.indices[K.index_count++] = idx;
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */
/* pushbuffer address of the word being executed (set by the host walker) */
uint32_t g_kelvin_pb_addr, g_kelvin_pb_in_call;

int kelvin_method(uint32_t subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel;
    if (!K.initialized) return 0;

    if (method == M_CONTEXT_DMA_A || method == M_CONTEXT_DMA_B) {
        uint32_t *slot = method == M_CONTEXT_DMA_A ? &K.dma_a : &K.dma_b;
        if (*slot != param)
            fprintf(stderr, "[KELVIN-DMA] %c handle=%08X pb=%08X\n",
                    method == M_CONTEXT_DMA_A ? 'A' : 'B', param, g_kelvin_pb_addr);
        *slot = param;
        return 1;
    }

    if (method >= M_TRANSFORM_DATA && method < M_TRANSFORM_DATA + 0x10) {
        K.transform_data[(method - M_TRANSFORM_DATA) / 4] = u2f(param);
        return 1;
    }
    if (method >= M_TRANSFORM_PROGRAM && method < M_TRANSFORM_PROGRAM + 0x80) {
        uint32_t slot = (method - M_TRANSFORM_PROGRAM) / 4;
        if (K.program_load < PROGRAM_SLOTS) {
            K.program[K.program_load][slot % 4] = param;
            g_vdec_ok[K.program_load] = 0;
        }
        if (slot % 4 == 3) K.program_load++;
        return 1;
    }
    if (method >= M_TRANSFORM_CONSTANT && method < M_TRANSFORM_CONSTANT + 0x80) {
        uint32_t slot = (method - M_TRANSFORM_CONSTANT) / 4;
        if (K.const_load < CONST_ROWS) K.constants[K.const_load][slot % 4] = u2f(param);
        if (slot % 4 == 3) K.const_load++;
        return 1;
    }
    if (method >= M_COMPOSITE_MATRIX && method < M_COMPOSITE_MATRIX + 0x40) {
        uint32_t slot = (method - M_COMPOSITE_MATRIX) / 4;
        K.composite[slot / 4][slot % 4] = u2f(param); /* row n/4, column n%4 */
        K.constants[0 + slot / 4][slot % 4] = u2f(param); /* XFCTX_CMAT0 */
        return 1;
    }
    if (method >= M_VIEWPORT_OFFSET && method < M_VIEWPORT_OFFSET + 0x10) {
        K.viewport_offset[(method - M_VIEWPORT_OFFSET) / 4] = u2f(param);
        K.constants[0x3B][(method - M_VIEWPORT_OFFSET) / 4] = u2f(param); /* XFCTX_VPOFF */
        return 1;
    }
    if (method >= M_VIEWPORT_SCALE && method < M_VIEWPORT_SCALE + 0x10) {
        K.viewport_scale[(method - M_VIEWPORT_SCALE) / 4] = u2f(param);
        K.constants[0x3A][(method - M_VIEWPORT_SCALE) / 4] = u2f(param); /* XFCTX_VPSCL */
        return 1;
    }
    if (method >= M_VERTEX_ARRAY_OFFSET && method < M_VERTEX_ARRAY_OFFSET + 0x40) {
        K.attr_offset[(method - M_VERTEX_ARRAY_OFFSET) / 4] = param & 0x7FFFFFFFu;
        return 1;
    }
    if (method >= M_VERTEX_ARRAY_FORMAT && method < M_VERTEX_ARRAY_FORMAT + 0x40) {
        K.attr_format[(method - M_VERTEX_ARRAY_FORMAT) / 4] = param;
        return 1;
    }
    if (method >= M_VERTEX_DATA4UB && method < M_VERTEX_DATA4UB + 0x40) {
        int a = (method - M_VERTEX_DATA4UB) / 4;
        K.attr_const[a][0] = (param & 0xFF) / 255.0f;
        K.attr_const[a][1] = ((param >> 8) & 0xFF) / 255.0f;
        K.attr_const[a][2] = ((param >> 16) & 0xFF) / 255.0f;
        K.attr_const[a][3] = ((param >> 24) & 0xFF) / 255.0f;
        return 1;
    }
    if (method >= M_VERTEX_DATA4F && method < M_VERTEX_DATA4F + 0x100) {
        uint32_t i = (method - M_VERTEX_DATA4F) / 4;
        K.attr_const[i / 4][i % 4] = u2f(param);
        return 1;
    }
    if (method >= M_VERTEX_DATA2F && method < M_VERTEX_DATA2F + 0x80) {
        uint32_t i = (method - M_VERTEX_DATA2F) / 4;
        K.attr_const[i / 2][i % 2] = u2f(param);
        K.attr_const[i / 2][2] = 0.0f; K.attr_const[i / 2][3] = 1.0f;
        return 1;
    }
    if (method >= M_TEXTURE_BASE && method < M_TEXTURE_BASE + 0x100) {
        uint32_t stage = (method - M_TEXTURE_BASE) / 0x40, reg = (method - M_TEXTURE_BASE) % 0x40;
        TexStage *t = &K.tex[stage];
        switch (reg) {
        case 0x00: t->offset = param; break;
        case 0x04: t->format = param; break;
        case 0x08: t->address = param; break;
        case 0x0C: t->control0 = param; break;
        case 0x10: t->control1 = param; break;
        case 0x14: t->filter = param; break;
        case 0x1C: t->image_rect = param; break;
        case 0x20: t->palette = param; break;
        case 0x28: case 0x2C: case 0x30: case 0x34: {
            /* BUMP_ENV_MAT: methods are 00,01,11,10 (xemu swizzle) */
            static const int col[4] = { 0, 1, 3, 2 };
            K.psh.bump_mat[stage][col[(reg - 0x28) / 4]] = u2f(param);
            break;
        }
        case 0x38: K.psh.bump_scale[stage] = u2f(param); break;
        case 0x3C: K.psh.bump_offset[stage] = u2f(param); break;
        default: break;
        }
        return 1;
    }

    /* register combiners (xemu pgraph: COMBINE* registers) */
    if (method >= 0x0260 && method < 0x0280) { K.psh.alpha_in[(method - 0x0260) / 4] = param; return 1; }
    if (method >= 0x0A60 && method < 0x0A80) { K.psh.factor0[(method - 0x0A60) / 4] = param; return 1; }
    if (method >= 0x0A80 && method < 0x0AA0) { K.psh.factor1[(method - 0x0A80) / 4] = param; return 1; }
    if (method >= 0x0AA0 && method < 0x0AC0) { K.psh.alpha_out[(method - 0x0AA0) / 4] = param; return 1; }
    if (method >= 0x0AC0 && method < 0x0AE0) { K.psh.rgb_in[(method - 0x0AC0) / 4] = param; return 1; }
    if (method >= 0x1E40 && method < 0x1E60) { K.psh.rgb_out[(method - 0x1E40) / 4] = param; return 1; }
    switch (method) {
    case 0x0288: K.psh.final0 = param; return 1;               /* SPECULAR_FOG_CW0 */
    case 0x028C: K.psh.final1 = param; return 1;               /* SPECULAR_FOG_CW1 */
    case 0x02A8: /* FOG_COLOR: parameter is ABGR, PGRAPH keeps ARGB */
        K.psh.fog_color = (param & 0xFF00FF00u) | ((param & 0xFFu) << 16) | ((param >> 16) & 0xFFu);
        return 1;
    case 0x1E20: K.psh.final_factor0 = param; return 1;        /* SPECULAR_FOG_FACTOR */
    case 0x1E24: K.psh.final_factor1 = param; return 1;
    case 0x1E60: K.psh.combiner_control = param; return 1;     /* COMBINER_CONTROL */
    case 0x1E70: K.psh.shader_stage_program = param; return 1; /* SHADER_STAGE_PROGRAM */
    case 0x1E78: K.psh.other_stage_input = param; return 1;    /* SHADER_OTHER_STAGE_INPUT */
    default: break;
    }

    switch (method) {
    case M_BEGIN_END:
        if (param == 0) {
            if (K.in_begin) end_primitive();
            K.in_begin = 0;
        } else {
            K.in_begin = 1;
            K.prim = param;
            K.inline_count = K.index_count = K.array_count = 0;
        }
        return 1;
    case M_INLINE_ARRAY:
        if (!K.inline_count) { g_inline_src = g_kelvin_pb_addr; g_inline_in_call = g_kelvin_pb_in_call; }
        if (K.inline_count < MAX_VERTS * 4) K.inline_data[K.inline_count++] = param;
        return 1;
    case M_ARRAY_ELEMENT16:
        push_index(param & 0xFFFF);
        push_index(param >> 16);
        return 1;
    case M_ARRAY_ELEMENT32:
        push_index(param);
        return 1;
    case M_DRAW_ARRAYS: {
        uint32_t start = param & 0xFFFFFF, count = (param >> 24) + 1;
        /* consecutive DRAW_ARRAYS extend one run; otherwise emit indices */
        if (K.array_count && K.array_start + K.array_count == start) {
            K.array_count += count;
        } else if (!K.array_count && !K.index_count) {
            K.array_start = start; K.array_count = count;
        } else {
            if (K.array_count) {
                for (uint32_t i = 0; i < K.array_count; ++i) push_index(K.array_start + i);
                K.array_count = 0;
            }
            for (uint32_t i = 0; i < count; ++i) push_index(start + i);
        }
        return 1;
    }
    case M_TRANSFORM_EXEC_MODE: K.program_mode = (param & 3) == 2; return 1;
    case M_LAUNCH_TRANSFORM: {
        Vec4 input[16] = {{{0}}};
        VshState state;
        memcpy(input[0].v, K.transform_data, sizeof(K.transform_data));
        vsh_run_from(param, input, &state, 1);
        return 1;
    }
    case M_TRANSFORM_PROG_LOAD: K.program_load = param; return 1;
    case M_TRANSFORM_PROG_START: K.program_start = param; return 1;
    case M_TRANSFORM_CONST_LOAD: K.const_load = param; return 1;
    case M_CLIP_MIN: K.clip_min = u2f(param); return 1;
    case M_CLIP_MAX: K.clip_max = u2f(param); return 1;
    case M_BLEND_ENABLE: K.blend_enable = param; return 1;
    case M_BLEND_SFACTOR: K.sfactor = param; return 1;
    case M_BLEND_DFACTOR: K.dfactor = param; return 1;
    case M_BLEND_EQUATION: K.blend_eq = param; return 1;
    case M_ALPHA_TEST_ENABLE: K.alpha_test = param; return 1;
    case M_ALPHA_FUNC: K.alpha_func = param; return 1;
    case M_ALPHA_REF: K.alpha_ref = param; return 1;
    case M_DEPTH_TEST_ENABLE: K.depth_test = param; return 1;
    case M_DEPTH_FUNC: K.depth_func = param; return 1;
    case M_DEPTH_MASK: K.depth_mask = param; return 1;
    case M_COLOR_MASK: K.color_mask = param; return 1;
    case M_SURFACE_COLOR_OFFSET:
        g_surf_color = param & 0x03FFFFFFu;
        select_target();
        return 1;
    case M_SURFACE_CLIP_H:
        K.clip_h = param;
        if (g_target_key) d3d8_rt_select(g_target_key, K.clip_h >> 16, K.clip_v >> 16);
        return 1;
    case M_SURFACE_CLIP_V:
        K.clip_v = param;
        if (g_target_key) d3d8_rt_select(g_target_key, K.clip_h >> 16, K.clip_v >> 16);
        return 1;
    case M_COLOR_CLEAR_VALUE: K.clear_color = param; return 1;
    case M_ZSTENCIL_CLEAR_VALUE: K.clear_zstencil = param; return 1;
    case M_CLEAR_RECT_H: K.clear_rect_h = param; return 1;
    case M_CLEAR_RECT_V: K.clear_rect_v = param; return 1;
    case M_CLEAR_SURFACE: {
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();
        if (kenv("KELVIN_LOG_CLEARS")) {
            fprintf(stderr,
                    "[CLEAR] frame=%u flags=%08X rect=(%u,%u)-(%u,%u) color=%08X zs=%08X surf=%08X clip=%08X/%08X\n",
                    K.frame, param,
                    K.clear_rect_h & 0xFFFF, K.clear_rect_v & 0xFFFF,
                    (K.clear_rect_h >> 16) + 1, (K.clear_rect_v >> 16) + 1,
                    K.clear_color, K.clear_zstencil, g_surf_color, K.clip_h, K.clip_v);
        }
        if (dev) {
            DWORD flags = 0;
            D3DRECT rect;
            rect.x1 = K.clear_rect_h & 0xFFFF; rect.x2 = (K.clear_rect_h >> 16) + 1;
            rect.y1 = K.clear_rect_v & 0xFFFF; rect.y2 = (K.clear_rect_v >> 16) + 1;
            if (param & 0xF0) flags |= D3DCLEAR_TARGET;
            if (param & 0x01) flags |= D3DCLEAR_ZBUFFER;
            if (param & 0x02) flags |= D3DCLEAR_STENCIL;
            dev->lpVtbl->Clear(dev, 1, &rect, flags, K.clear_color,
                               (float)(K.clear_zstencil >> 8) / 16777215.0f,
                               K.clear_zstencil & 0xFF);
        }
        K.stats.clears++;
        return 1;
    }
    default:
        return 0;
    }
}

void kelvin_end_frame(void)
{
    if (K.in_begin) { end_primitive(); K.in_begin = 0; }
    if (g_surf_color && g_surf_color != g_display[0]) {
        g_display[1] = g_display[0];
        g_display[0] = g_surf_color;
    }
    if (g_target_key) {
        uint32_t completed_key = g_target_key;
        /* Initial history replay can classify the first completed display
         * surface as offscreen.  Present only this frame-ending surface; never
         * expose intermediate render-to-texture passes, which visibly flash. */
        g_target_key = 0;
        d3d8_rt_present(completed_key);
        d3d8_rt_select(0, 0, 0);
    }
    K.frame++;
}
