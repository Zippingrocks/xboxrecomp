/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (c) 2026 xboxrecomp contributors
 *
 * This file is a source-level port of xemu's GPL-licensed NV2A register
 * combiner implementation and is distributed under GPL-2.0-or-later.
 */

/**
 * NV2A register combiners from raw PGRAPH register words -> HLSL.
 *
 * A port of xemu's hw/xbox/nv2a/pgraph/glsl/psh.c (pgraph_glsl_gen_psh /
 * psh_convert / add_stage_code / add_final_stage_code) to HLSL, fed directly
 * with the NV097 method values the pushbuffer translator sees
 * (SET_COMBINER_*, SET_SHADER_STAGE_PROGRAM, SET_SHADER_OTHER_STAGE_INPUT,
 * SET_SPECULAR_FOG_FACTOR). Kept deliberately close to xemu so behaviour can
 * be compared line by line. Not handled (same as the pretransformed vertex
 * path cannot supply them): 3D/cube textures (sampled as 2D), DOT_* texture
 * modes, clip-plane discards, colour keys, point sprites.
 */

#include "d3d8_internal.h"
#include "d3d8_nv2a_psh.h"
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* string builder                                                    */
/* ---------------------------------------------------------------- */

typedef struct { char *p; size_t n, cap; } SB;

static void sb_fmt(SB *s, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (s->n >= s->cap) return;
    va_start(ap, fmt);
    k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
    va_end(ap);
    if (k > 0) s->n += (size_t)k < s->cap - s->n ? (size_t)k : s->cap - s->n - 1;
}

/* ---------------------------------------------------------------- */
/* xemu psh.c structures                                             */
/* ---------------------------------------------------------------- */

typedef struct { int reg, chan, mod; } InputInfo;
typedef struct { InputInfo a, b, c, d; } InputVarInfo;
typedef struct {
    int ab, cd, muxsum, flags, ab_op, cd_op, muxsum_op, mapping;
} OutputInfo;
typedef struct {
    int enabled, clamp_sum, inv_v1, inv_r0;
    InputInfo a, b, c, d, e, f, g;
} FCInputInfo;

typedef struct {
    int num_stages, flags, cur_stage;
    int tex_modes[4], input_tex[4];
    struct { InputVarInfo rgb_input, alpha_input; OutputInfo rgb_output, alpha_output; } stage[8];
    FCInputInfo final_input;
    char varE[256], varF[256];
} PixelShader;

static void parse_input(InputInfo *v, int value)
{
    v->reg = value & 0xF;
    v->chan = value & 0x10;
    v->mod = value & 0xE0;
}

static void parse_combiner_inputs(uint32_t value, InputInfo *a, InputInfo *b,
                                  InputInfo *c, InputInfo *d)
{
    parse_input(d, value & 0xFF);
    parse_input(c, (value >> 8) & 0xFF);
    parse_input(b, (value >> 16) & 0xFF);
    parse_input(a, (value >> 24) & 0xFF);
}

static void parse_combiner_output(uint32_t value, OutputInfo *out)
{
    int flags = (int)(value >> 12);
    out->cd = value & 0xF;
    out->ab = (value >> 4) & 0xF;
    out->muxsum = (value >> 8) & 0xF;
    out->flags = flags;
    out->cd_op = flags & 1;
    out->ab_op = flags & 2;
    out->muxsum_op = flags & 4;
    out->mapping = flags & 0x38;
}

/* get_var: register name (dest) or expression (source) */
static void get_var(PixelShader *ps, int reg, int is_dest, char *out, size_t n)
{
    switch (reg) {
    case 0x0: snprintf(out, n, "%s", is_dest ? "" : "float4(0.0, 0.0, 0.0, 0.0)"); break;
    case 0x1:
        if ((ps->flags & 0x10) || ps->cur_stage == 8) snprintf(out, n, "consts[%d]", ps->cur_stage * 2);
        else snprintf(out, n, "consts[0]");
        break;
    case 0x2:
        if ((ps->flags & 0x100) || ps->cur_stage == 8) snprintf(out, n, "consts[%d]", ps->cur_stage * 2 + 1);
        else snprintf(out, n, "consts[1]");
        break;
    case 0x3: snprintf(out, n, "pFog"); break;
    case 0x4: snprintf(out, n, "v0"); break;
    case 0x5: snprintf(out, n, "v1"); break;
    case 0x8: case 0x9: case 0xA: case 0xB: snprintf(out, n, "t%d", reg - 8); break;
    case 0xC: snprintf(out, n, "r0"); break;
    case 0xD: snprintf(out, n, "r1"); break;
    case 0xE:
        snprintf(out, n, ps->final_input.clamp_sum
                 ? "saturate(float4(%s.rgb + %s.rgb, 0.0))" : "float4(%s.rgb + %s.rgb, 0.0)",
                 ps->final_input.inv_v1 ? "(1.0 - v1)" : "v1",
                 ps->final_input.inv_r0 ? "(1.0 - r0)" : "r0");
        break;
    case 0xF: snprintf(out, n, "float4(%s * %s, 0.0)", ps->varE, ps->varF); break;
    default: snprintf(out, n, "float4(0.0, 0.0, 0.0, 0.0)"); break;
    }
}

static void get_input_var(PixelShader *ps, InputInfo in, int is_alpha, char *out, size_t n)
{
    char reg[256], r[300];
    get_var(ps, in.reg, 0, reg, sizeof(reg));
    if (!is_alpha) snprintf(r, sizeof(r), "(%s).%s", reg, in.chan ? "aaa" : "rgb");
    else snprintf(r, sizeof(r), "(%s).%s", reg, in.chan ? "a" : "b");
    switch (in.mod) {
    case 0x00: snprintf(out, n, "max(%s, 0.0)", r); break;
    case 0x20: snprintf(out, n, "(1.0 - saturate(%s))", r); break;
    case 0x40: snprintf(out, n, "(2.0 * max(%s, 0.0) - 1.0)", r); break;
    case 0x60: snprintf(out, n, "(-2.0 * max(%s, 0.0) + 1.0)", r); break;
    case 0x80: snprintf(out, n, "(max(%s, 0.0) - 0.5)", r); break;
    case 0xA0: snprintf(out, n, "(-max(%s, 0.0) + 0.5)", r); break;
    case 0xC0: snprintf(out, n, "%s", r); break;
    default:   snprintf(out, n, "(-%s)", r); break;
    }
}

static void get_output(const char *reg, int mapping, char *out, size_t n)
{
    switch (mapping) {
    case 0x08: snprintf(out, n, "(%s - 0.5)", reg); break;
    case 0x10: snprintf(out, n, "(%s * 2.0)", reg); break;
    case 0x18: snprintf(out, n, "((%s - 0.5) * 2.0)", reg); break;
    case 0x20: snprintf(out, n, "(%s * 4.0)", reg); break;
    case 0x30: snprintf(out, n, "(%s / 2.0)", reg); break;
    default:   snprintf(out, n, "%s", reg); break;
    }
}

static void add_stage_code(PixelShader *ps, SB *code, SB *ret, InputVarInfo input,
                           OutputInfo output, const char *mask, int is_alpha)
{
    char a[400], b[400], c[400], d[400], ab[900], cd[900], abm[1000], cdm[1000];
    char abd[64], cdd[64], msd[64], ms[1900], msm[2000];
    const char *caster = strlen(mask) == 3 ? "(float3)" : "";
    int assign_ab = 0, assign_cd = 0, assign_ms = 0;

    get_input_var(ps, input.a, is_alpha, a, sizeof(a));
    get_input_var(ps, input.b, is_alpha, b, sizeof(b));
    get_input_var(ps, input.c, is_alpha, c, sizeof(c));
    get_input_var(ps, input.d, is_alpha, d, sizeof(d));
    if (output.ab_op) snprintf(ab, sizeof(ab), "dot(%s, %s)", a, b);
    else snprintf(ab, sizeof(ab), "(%s * %s)", a, b);
    if (output.cd_op) snprintf(cd, sizeof(cd), "dot(%s, %s)", c, d);
    else snprintf(cd, sizeof(cd), "(%s * %s)", c, d);
    get_output(ab, output.mapping, abm, sizeof(abm));
    get_output(cd, output.mapping, cdm, sizeof(cdm));
    get_var(ps, output.ab, 1, abd, sizeof(abd));
    get_var(ps, output.cd, 1, cdd, sizeof(cdd));
    get_var(ps, output.muxsum, 1, msd, sizeof(msd));

    if (abd[0]) { sb_fmt(code, "ab.%s = clamp(%s(%s), -1.0, 1.0);\n", mask, caster, abm); assign_ab = 1; }
    if (cdd[0]) { sb_fmt(code, "cd.%s = clamp(%s(%s), -1.0, 1.0);\n", mask, caster, cdm); assign_cd = 1; }

    if (!output.muxsum_op) snprintf(ms, sizeof(ms), "(%s + %s)", ab, cd);
    else snprintf(ms, sizeof(ms), "((%s) ? %s(%s) : %s(%s))",
                  (ps->flags & 0x1) ? "r0.a >= 0.5" : "(((uint)(r0.a * 255.0)) & 1u) == 1u",
                  caster, cd, caster, ab);
    get_output(ms, output.mapping, msm, sizeof(msm));
    if (msd[0]) { sb_fmt(code, "mux_sum.%s = clamp(%s(%s), -1.0, 1.0);\n", mask, caster, msm); assign_ms = 1; }

    if (assign_ab) {
        sb_fmt(ret, "%s.%s = ab.%s;\n", abd, mask, mask);
        if (!is_alpha && (output.flags & 0x80)) sb_fmt(ret, "%s.a = ab.b;\n", abd);
    }
    if (assign_cd) {
        sb_fmt(ret, "%s.%s = cd.%s;\n", cdd, mask, mask);
        if (!is_alpha && (output.flags & 0x40)) sb_fmt(ret, "%s.a = cd.b;\n", cdd);
    }
    if (assign_ms) sb_fmt(ret, "%s.%s = mux_sum.%s;\n", msd, mask, mask);
}

static void add_final_stage_code(PixelShader *ps, SB *code, FCInputInfo f)
{
    char a[400], b[400], c[400], d[400], g[400];
    get_input_var(ps, f.e, 0, ps->varE, sizeof(ps->varE));
    get_input_var(ps, f.f, 0, ps->varF, sizeof(ps->varF));
    get_input_var(ps, f.a, 0, a, sizeof(a));
    get_input_var(ps, f.b, 0, b, sizeof(b));
    get_input_var(ps, f.c, 0, c, sizeof(c));
    get_input_var(ps, f.d, 0, d, sizeof(d));
    get_input_var(ps, f.g, 1, g, sizeof(g));
    sb_fmt(code, "fragColor.rgb = %s + lerp((float3)(%s), (float3)(%s), (float3)(%s));\n", d, c, b, a);
    sb_fmt(code, "fragColor.a = %s;\n", g);
}

static int generate(const NV2APshRegs *r, char *out, size_t cap)
{
    PixelShader ps;
    SB s = { out, 0, cap };
    char codebuf[24576], stagebuf[8192];
    SB code = { codebuf, 0, sizeof(codebuf) };
    int i;

    memset(&ps, 0, sizeof(ps));
    codebuf[0] = 0;
    ps.num_stages = r->combiner_control & 0xFF;
    if (ps.num_stages > 8) ps.num_stages = 8;
    ps.flags = (int)(r->combiner_control >> 8);
    for (i = 0; i < 4; i++) ps.tex_modes[i] = (r->shader_stage_program >> (i * 5)) & 0x1F;
    ps.input_tex[0] = -1;
    ps.input_tex[1] = 0;
    ps.input_tex[2] = (r->other_stage_input >> 16) & 0xF;
    ps.input_tex[3] = (r->other_stage_input >> 20) & 0xF;
    for (i = 0; i < ps.num_stages; i++) {
        parse_combiner_inputs(r->rgb_in[i], &ps.stage[i].rgb_input.a, &ps.stage[i].rgb_input.b,
                              &ps.stage[i].rgb_input.c, &ps.stage[i].rgb_input.d);
        parse_combiner_inputs(r->alpha_in[i], &ps.stage[i].alpha_input.a, &ps.stage[i].alpha_input.b,
                              &ps.stage[i].alpha_input.c, &ps.stage[i].alpha_input.d);
        parse_combiner_output(r->rgb_out[i], &ps.stage[i].rgb_output);
        parse_combiner_output(r->alpha_out[i], &ps.stage[i].alpha_output);
    }
    ps.final_input.enabled = r->final0 || r->final1;
    if (ps.final_input.enabled) {
        InputInfo blank;
        int fl = r->final1 & 0xFF;
        parse_combiner_inputs(r->final0, &ps.final_input.a, &ps.final_input.b,
                              &ps.final_input.c, &ps.final_input.d);
        parse_combiner_inputs(r->final1, &ps.final_input.e, &ps.final_input.f,
                              &ps.final_input.g, &blank);
        ps.final_input.clamp_sum = fl & 0x80;
        ps.final_input.inv_v1 = fl & 0x40;
        ps.final_input.inv_r0 = fl & 0x20;
    }

    sb_fmt(&s,
        "cbuffer NV2APsh : register(b0) {\n"
        "    float4 consts[18];\n"
        "    float4 fogColor;\n"
        "    float4 bumpMat[4];\n"
        "    float4 bumpScaleOffset[4];\n"
        "    float4 alphaRefPad;\n"
        "};\n"
        "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);\n"
        "Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
        "SamplerState samp0 : register(s0); SamplerState samp1 : register(s1);\n"
        "SamplerState samp2 : register(s2); SamplerState samp3 : register(s3);\n"
        "TextureCube cube0 : register(t4); TextureCube cube1 : register(t5);\n"
        "TextureCube cube2 : register(t6); TextureCube cube3 : register(t7);\n"
        "struct PS_IN {\n"
        "    float4 pos : SV_POSITION; float4 diffuse : COLOR0; float4 specular : COLOR1;\n"
        "    float4 tex0 : TEXCOORD0; float4 tex1 : TEXCOORD1; float4 tex2 : TEXCOORD2;\n"
        "    float4 tex3 : TEXCOORD3; float fog : TEXCOORD4;\n"
        "};\n"
        "float sign3(float x) { return x >= 0.5 ? x - 1.0 : x; }\n"
        "float4 main(PS_IN input) : SV_TARGET {\n"
        "float4 pD0 = input.diffuse;\n"
        "float4 pD1 = input.specular;\n"
        "float4 pFog = float4(fogColor.rgb, saturate(input.fog));\n"
        "float4 pT0 = input.tex0;\n"
        "float4 pT1 = input.tex1;\n"
        "float4 pT2 = input.tex2;\n"
        "float4 pT3 = input.tex3;\n"
        "float4 v0 = pD0;\n"
        "float4 v1 = pD1;\n"
        "float4 ab = 0.0, cd = 0.0, mux_sum = 0.0;\n"
        "float4 fragColor = 0.0;\n");

    for (i = 0; i < 4; i++) {
        int in = ps.input_tex[i];
        switch (ps.tex_modes[i]) {
        case 0x01: case 0x02:  /* PROJECT2D / PROJECT3D (3D textures sampled as 2D) */
            sb_fmt(&s, "float4 t%d = tex%d.Sample(samp%d, pT%d.xy);\n", i, i, i, i);
            break;
        case 0x03:  /* CUBEMAP */
            if (r->cube_mask & (1u << i))
                sb_fmt(&s, "float4 t%d = cube%d.Sample(samp%d, pT%d.xyz);\n", i, i, i, i);
            else
                sb_fmt(&s, "float4 t%d = tex%d.Sample(samp%d, pT%d.xy);\n", i, i, i, i);
            break;
        case 0x04:  /* PASSTHRU */
            sb_fmt(&s, "float4 t%d = pT%d;\n", i, i);
            break;
        case 0x06:  /* BUMPENVMAP */
            if (in < 0) in = 0;
            sb_fmt(&s,
                "float2 dsdt%d = float2(sign3(t%d.b), sign3(t%d.g));\n"
                "dsdt%d = float2(dot(bumpMat[%d].xz, dsdt%d), dot(bumpMat[%d].yw, dsdt%d));\n"
                "float4 t%d = tex%d.Sample(samp%d, pT%d.xy + dsdt%d);\n",
                i, in, in, i, i, i, i, i, i, i, i, i, i);
            break;
        case 0x07:  /* BUMPENVMAP_LUM */
            if (in < 0) in = 0;
            sb_fmt(&s,
                "float3 dsdtl%d = float3(sign3(t%d.b), sign3(t%d.g), t%d.r);\n"
                "dsdtl%d.xy = float2(dot(bumpMat[%d].xz, dsdtl%d.xy), dot(bumpMat[%d].yw, dsdtl%d.xy));\n"
                "float4 t%d = tex%d.Sample(samp%d, pT%d.xy + dsdtl%d.xy);\n"
                "t%d = t%d * (bumpScaleOffset[%d].x * dsdtl%d.z + bumpScaleOffset[%d].y);\n",
                i, in, in, in, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i);
            break;
        case 0x0F:  /* DPNDNT_AR */
            if (in < 0) in = 0;
            sb_fmt(&s, "float4 t%d = tex%d.Sample(samp%d, t%d.ar);\n", i, i, i, in);
            break;
        case 0x10:  /* DPNDNT_GB */
            if (in < 0) in = 0;
            sb_fmt(&s, "float4 t%d = tex%d.Sample(samp%d, t%d.gb);\n", i, i, i, in);
            break;
        case 0x00:
            sb_fmt(&s, "float4 t%d = float4(0.0, 0.0, 0.0, 1.0);\n", i);
            break;
        default:    /* CLIPPLANE, BRDF, DOT_* : not emulated */
            sb_fmt(&s, "float4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n", i);
            break;
        }
    }
    sb_fmt(&s, "float4 r0 = 0.0, r1 = 0.0;\nr0.a = %s;\n",
           ps.tex_modes[0] != 0 ? "t0.a" : "1.0");

    for (i = 0; i < ps.num_stages; i++) {
        SB ret = { stagebuf, 0, sizeof(stagebuf) };
        stagebuf[0] = 0;
        ps.cur_stage = i;
        sb_fmt(&code, "// Stage %d\n", i);
        add_stage_code(&ps, &code, &ret, ps.stage[i].rgb_input, ps.stage[i].rgb_output, "rgb", 0);
        add_stage_code(&ps, &code, &ret, ps.stage[i].alpha_input, ps.stage[i].alpha_output, "a", 1);
        sb_fmt(&code, "%s", stagebuf);
    }
    if (ps.final_input.enabled) {
        ps.cur_stage = 8;
        sb_fmt(&code, "// Final Combiner\n");
        add_final_stage_code(&ps, &code, ps.final_input);
    } else {
        sb_fmt(&code, "fragColor = r0;\n");
    }
    if (r->alpha_test && r->alpha_func != 0x207) {
        static const char *ops[8] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
        int f = (int)(r->alpha_func & 7);
        if ((r->alpha_func & 0xF) == 0 || f == 0) sb_fmt(&code, "discard;\n");
        else sb_fmt(&code, "if (!(((int)round(fragColor.a * 255.0)) %s (int)alphaRefPad.x)) discard;\n", ops[f]);
    }
    sb_fmt(&s, "%s", codebuf);
    sb_fmt(&s, "return fragColor;\n}\n");
    return s.n + 1 < s.cap ? (int)s.n : -1;
}

/* ---------------------------------------------------------------- */
/* cache + draw integration                                          */
/* ---------------------------------------------------------------- */

#define PSH_CACHE 256
typedef struct { NV2APshRegs key; ID3D11PixelShader *ps; int used; } PshEntry;
static PshEntry g_psh[PSH_CACHE];
static NV2APshRegs g_cur;
static int g_cur_active;
static ID3D11Buffer *g_psh_cb;

typedef struct {
    float consts[18][4];
    float fog[4];
    float bump[4][4];
    float bump_so[4][4];
    float alpha_ref[4];
} PshCB;

/* fields that select the shader; factors/fog/bump go in the cbuffer */
static void shader_key(const NV2APshRegs *r, NV2APshRegs *k)
{
    int n = r->combiner_control & 0xFF;
    memset(k, 0, sizeof(*k));
    k->combiner_control = r->combiner_control;
    k->shader_stage_program = r->shader_stage_program;
    k->other_stage_input = r->other_stage_input;
    k->final0 = r->final0;
    k->final1 = r->final1;
    for (int i = 0; i < n && i < 8; i++) {
        k->rgb_in[i] = r->rgb_in[i]; k->rgb_out[i] = r->rgb_out[i];
        k->alpha_in[i] = r->alpha_in[i]; k->alpha_out[i] = r->alpha_out[i];
    }
    k->cube_mask = r->cube_mask;
    k->alpha_test = r->alpha_test;
    k->alpha_func = r->alpha_test ? r->alpha_func : 0;
}

static ID3D11PixelShader *get_shader(const NV2APshRegs *r)
{
    NV2APshRegs key;
    static char hlsl[65536];
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D11PixelShader *ps = NULL;
    int len, slot = -1;

    shader_key(r, &key);
    for (int i = 0; i < PSH_CACHE; i++) {
        if (g_psh[i].used && !memcmp(&g_psh[i].key, &key, sizeof(key))) return g_psh[i].ps;
        if (!g_psh[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return NULL;
    len = generate(&key, hlsl, sizeof(hlsl));
    if (len < 0) return NULL;
    if (FAILED(D3DCompile(hlsl, (SIZE_T)len, "nv2a_psh", NULL, NULL, "main", "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &err))) {
        static int shown;
        if (shown++ < 4)
            fprintf(stderr, "[NV2A-PSH] compile failed: %s\n--- HLSL ---\n%s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", hlsl);
        if (err) ID3D10Blob_Release(err);
        g_psh[slot].key = key; g_psh[slot].ps = NULL; g_psh[slot].used = 1;
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    ID3D11Device_CreatePixelShader(d3d8_GetD3D11Device(), ID3D10Blob_GetBufferPointer(blob),
                                   ID3D10Blob_GetBufferSize(blob), NULL, &ps);
    ID3D10Blob_Release(blob);
    g_psh[slot].key = key; g_psh[slot].ps = ps; g_psh[slot].used = 1;
    if (getenv("NV2A_PSH_DUMP")) {
        static int dumped;
        if (dumped++ < 64) fprintf(stderr, "[NV2A-PSH] #%d\n%s\n", slot, hlsl);
    }
    return ps;
}

#define CUBE_CACHE 32
typedef struct { uint64_t key; ID3D11ShaderResourceView *srv; ID3D11Texture2D *tex; unsigned last; } CubeEntry;
static CubeEntry g_cubes[CUBE_CACHE];
static ID3D11ShaderResourceView *g_stage_cube[4];
static unsigned g_cube_tick;

int d3d8_nv2a_psh_set_cube(int stage, const uint32_t *argb_faces, uint32_t size, uint64_t key)
{
    CubeEntry *victim = &g_cubes[0];
    if (stage < 0 || stage > 3) return 0;
    g_stage_cube[stage] = NULL;
    if (!key) return 0;
    g_cube_tick++;
    for (int i = 0; i < CUBE_CACHE; i++) {
        if (g_cubes[i].srv && g_cubes[i].key == key) {
            g_cubes[i].last = g_cube_tick;
            g_stage_cube[stage] = g_cubes[i].srv;
            return 1;
        }
        if (!g_cubes[i].srv || g_cubes[i].last < victim->last) victim = &g_cubes[i];
    }
    if (!argb_faces || !size) return 0;
    if (victim->srv) { ID3D11ShaderResourceView_Release(victim->srv); victim->srv = NULL; }
    if (victim->tex) { ID3D11Texture2D_Release(victim->tex); victim->tex = NULL; }
    {
        D3D11_TEXTURE2D_DESC d;
        D3D11_SUBRESOURCE_DATA init[6];
        D3D11_SHADER_RESOURCE_VIEW_DESC v;
        memset(&d, 0, sizeof(d));
        d.Width = d.Height = size; d.MipLevels = 1; d.ArraySize = 6;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        for (int f = 0; f < 6; f++) {
            init[f].pSysMem = argb_faces + (size_t)f * size * size;
            init[f].SysMemPitch = size * 4;
            init[f].SysMemSlicePitch = 0;
        }
        if (FAILED(ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(), &d, init, &victim->tex))) return 0;
        memset(&v, 0, sizeof(v));
        v.Format = d.Format;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        v.TextureCube.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(d3d8_GetD3D11Device(),
                   (ID3D11Resource *)victim->tex, &v, &victim->srv))) return 0;
    }
    victim->key = key;
    victim->last = g_cube_tick;
    g_stage_cube[stage] = victim->srv;
    return 1;
}

void d3d8_nv2a_psh_set(const NV2APshRegs *r)
{
    if (!r) { g_cur_active = 0; return; }
    g_cur = *r;
    g_cur_active = 1;
}

static void argb_to_rgba(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = ((c >> 24) & 0xFF) / 255.0f;
}

BOOL d3d8_nv2a_psh_prepare_draw(void)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11PixelShader *ps;
    D3D11_MAPPED_SUBRESOURCE m;
    if (!g_cur_active || !ctx) return FALSE;
    for (int i = 0; i < 4; i++)
        if (!g_stage_cube[i]) g_cur.cube_mask &= ~(1u << i);
    ps = get_shader(&g_cur);
    if (!ps) return FALSE;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 4, 4, g_stage_cube);
    if (!g_psh_cb) {
        D3D11_BUFFER_DESC d;
        memset(&d, 0, sizeof(d));
        d.ByteWidth = (sizeof(PshCB) + 15) & ~15u;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &d, NULL, &g_psh_cb))) return FALSE;
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_psh_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        PshCB *cb = (PshCB *)m.pData;
        memset(cb, 0, sizeof(*cb));
        for (int i = 0; i < 8; i++) {
            argb_to_rgba(g_cur.factor0[i], cb->consts[i * 2]);
            argb_to_rgba(g_cur.factor1[i], cb->consts[i * 2 + 1]);
        }
        argb_to_rgba(g_cur.final_factor0, cb->consts[16]);
        argb_to_rgba(g_cur.final_factor1, cb->consts[17]);
        argb_to_rgba(g_cur.fog_color, cb->fog);
        for (int i = 0; i < 4; i++) {
            memcpy(cb->bump[i], g_cur.bump_mat[i], sizeof(cb->bump[i]));
            cb->bump_so[i][0] = g_cur.bump_scale[i];
            cb->bump_so[i][1] = g_cur.bump_offset[i];
        }
        cb->alpha_ref[0] = (float)(g_cur.alpha_ref & 0xFF);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_psh_cb, 0);
    }
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_psh_cb);
    return TRUE;
}
