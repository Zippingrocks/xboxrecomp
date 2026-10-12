/**
 * MMX / SSE-integer helpers for recompiled code.
 *
 * Each helper takes and returns the 64-bit register image, operating lane by
 * lane with the exact wrap/saturate/shift semantics of the instruction it is
 * named after. Lanes are little-endian: lane 0 is the least significant.
 * Included from recomp_types.h.
 */
#ifndef RECOMP_MMX_H
#define RECOMP_MMX_H

#include <stdint.h>

typedef union {
    uint64_t q;
    uint32_t d[2];
    int32_t sd[2];
    uint16_t w[4];
    int16_t sw[4];
    uint8_t b[8];
    int8_t sb[8];
} RecompMmx;

static inline int16_t mmx_sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline int8_t mmx_sat8(int32_t v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
static inline uint16_t mmx_usat16(int32_t v) { return (uint16_t)(v > 65535 ? 65535 : v < 0 ? 0 : v); }
static inline uint8_t mmx_usat8(int32_t v) { return (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }

#define MMX_LANEWISE(name, field, n, expr)                                   \
    static inline uint64_t name(uint64_t a_, uint64_t b_) {                   \
        RecompMmx a, b, r; int i; a.q = a_; b.q = b_;                          \
        for (i = 0; i < (n); i++) r.field[i] = (expr);                         \
        return r.q;                                                            \
    }

/* Wrapping add/sub */
MMX_LANEWISE(mmx_paddb, b, 8, (uint8_t)(a.b[i] + b.b[i]))
MMX_LANEWISE(mmx_paddw, w, 4, (uint16_t)(a.w[i] + b.w[i]))
MMX_LANEWISE(mmx_paddd, d, 2, a.d[i] + b.d[i])
MMX_LANEWISE(mmx_psubb, b, 8, (uint8_t)(a.b[i] - b.b[i]))
MMX_LANEWISE(mmx_psubw, w, 4, (uint16_t)(a.w[i] - b.w[i]))
MMX_LANEWISE(mmx_psubd, d, 2, a.d[i] - b.d[i])
static inline uint64_t mmx_paddq(uint64_t a, uint64_t b) { return a + b; }
static inline uint64_t mmx_psubq(uint64_t a, uint64_t b) { return a - b; }

/* Saturating add/sub */
MMX_LANEWISE(mmx_paddsb, sb, 8, mmx_sat8(a.sb[i] + b.sb[i]))
MMX_LANEWISE(mmx_paddsw, sw, 4, mmx_sat16(a.sw[i] + b.sw[i]))
MMX_LANEWISE(mmx_paddusb, b, 8, mmx_usat8(a.b[i] + b.b[i]))
MMX_LANEWISE(mmx_paddusw, w, 4, mmx_usat16(a.w[i] + b.w[i]))
MMX_LANEWISE(mmx_psubsb, sb, 8, mmx_sat8(a.sb[i] - b.sb[i]))
MMX_LANEWISE(mmx_psubsw, sw, 4, mmx_sat16(a.sw[i] - b.sw[i]))
MMX_LANEWISE(mmx_psubusb, b, 8, mmx_usat8(a.b[i] - b.b[i]))
MMX_LANEWISE(mmx_psubusw, w, 4, mmx_usat16(a.w[i] - b.w[i]))

/* Multiplies */
MMX_LANEWISE(mmx_pmullw, w, 4, (uint16_t)((int32_t)a.sw[i] * b.sw[i]))
MMX_LANEWISE(mmx_pmulhw, w, 4, (uint16_t)(((int32_t)a.sw[i] * b.sw[i]) >> 16))
MMX_LANEWISE(mmx_pmulhuw, w, 4, (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16))
MMX_LANEWISE(mmx_pmaddwd, d, 2,
             (uint32_t)((int32_t)a.sw[2 * i] * b.sw[2 * i] +
                        (int32_t)a.sw[2 * i + 1] * b.sw[2 * i + 1]))
static inline uint64_t mmx_pmuludq(uint64_t a, uint64_t b) {
    return (uint64_t)(uint32_t)a * (uint32_t)b;
}

/* Bitwise */
static inline uint64_t mmx_pand(uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mmx_pandn(uint64_t a, uint64_t b) { return ~a & b; }
static inline uint64_t mmx_por(uint64_t a, uint64_t b) { return a | b; }
static inline uint64_t mmx_pxor(uint64_t a, uint64_t b) { return a ^ b; }

/* Compares */
MMX_LANEWISE(mmx_pcmpeqb, b, 8, a.b[i] == b.b[i] ? 0xFFu : 0u)
MMX_LANEWISE(mmx_pcmpeqw, w, 4, a.w[i] == b.w[i] ? 0xFFFFu : 0u)
MMX_LANEWISE(mmx_pcmpeqd, d, 2, a.d[i] == b.d[i] ? 0xFFFFFFFFu : 0u)
MMX_LANEWISE(mmx_pcmpgtb, b, 8, a.sb[i] > b.sb[i] ? 0xFFu : 0u)
MMX_LANEWISE(mmx_pcmpgtw, w, 4, a.sw[i] > b.sw[i] ? 0xFFFFu : 0u)
MMX_LANEWISE(mmx_pcmpgtd, d, 2, a.sd[i] > b.sd[i] ? 0xFFFFFFFFu : 0u)

/* Averages, min/max, sum of absolute differences (SSE integer on MMX) */
MMX_LANEWISE(mmx_pavgb, b, 8, (uint8_t)(((uint32_t)a.b[i] + b.b[i] + 1) >> 1))
MMX_LANEWISE(mmx_pavgw, w, 4, (uint16_t)(((uint32_t)a.w[i] + b.w[i] + 1) >> 1))
MMX_LANEWISE(mmx_pminub, b, 8, a.b[i] < b.b[i] ? a.b[i] : b.b[i])
MMX_LANEWISE(mmx_pmaxub, b, 8, a.b[i] > b.b[i] ? a.b[i] : b.b[i])
MMX_LANEWISE(mmx_pminsw, sw, 4, a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i])
MMX_LANEWISE(mmx_pmaxsw, sw, 4, a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i])
static inline uint64_t mmx_psadbw(uint64_t a_, uint64_t b_) {
    RecompMmx a, b; uint32_t s = 0; int i; a.q = a_; b.q = b_;
    for (i = 0; i < 8; i++) s += a.b[i] > b.b[i] ? a.b[i] - b.b[i] : b.b[i] - a.b[i];
    return s;
}

/* Shifts: the count is the whole 64-bit source (or an immediate); counts at
 * or beyond the lane width clear the lane (arithmetic shifts fill with sign). */
#define MMX_SHIFT(name, field, n, bits, expr_small, expr_big)                \
    static inline uint64_t name(uint64_t a_, uint64_t count) {                \
        RecompMmx a, r; int i; a.q = a_;                                       \
        for (i = 0; i < (n); i++)                                              \
            r.field[i] = count < (bits) ? (expr_small) : (expr_big);           \
        return r.q;                                                            \
    }
MMX_SHIFT(mmx_psllw, w, 4, 16, (uint16_t)(a.w[i] << count), 0)
MMX_SHIFT(mmx_pslld, d, 2, 32, a.d[i] << count, 0)
MMX_SHIFT(mmx_psrlw, w, 4, 16, (uint16_t)(a.w[i] >> count), 0)
MMX_SHIFT(mmx_psrld, d, 2, 32, a.d[i] >> count, 0)
MMX_SHIFT(mmx_psraw, sw, 4, 16, (int16_t)(a.sw[i] >> count), (int16_t)(a.sw[i] >> 15))
MMX_SHIFT(mmx_psrad, sd, 2, 32, a.sd[i] >> count, a.sd[i] >> 31)
static inline uint64_t mmx_psllq(uint64_t a, uint64_t count) { return count < 64 ? a << count : 0; }
static inline uint64_t mmx_psrlq(uint64_t a, uint64_t count) { return count < 64 ? a >> count : 0; }

/* Unpacks: interleave the low (or high) halves of a and b, a first. */
static inline uint64_t mmx_punpcklbw(uint64_t a_, uint64_t b_) {
    RecompMmx a, b, r; int i; a.q = a_; b.q = b_;
    for (i = 0; i < 4; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; }
    return r.q;
}
static inline uint64_t mmx_punpckhbw(uint64_t a_, uint64_t b_) {
    RecompMmx a, b, r; int i; a.q = a_; b.q = b_;
    for (i = 0; i < 4; i++) { r.b[2 * i] = a.b[4 + i]; r.b[2 * i + 1] = b.b[4 + i]; }
    return r.q;
}
static inline uint64_t mmx_punpcklwd(uint64_t a_, uint64_t b_) {
    RecompMmx a, b, r; a.q = a_; b.q = b_;
    r.w[0] = a.w[0]; r.w[1] = b.w[0]; r.w[2] = a.w[1]; r.w[3] = b.w[1];
    return r.q;
}
static inline uint64_t mmx_punpckhwd(uint64_t a_, uint64_t b_) {
    RecompMmx a, b, r; a.q = a_; b.q = b_;
    r.w[0] = a.w[2]; r.w[1] = b.w[2]; r.w[2] = a.w[3]; r.w[3] = b.w[3];
    return r.q;
}
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b) {
    return (a & 0xFFFFFFFFull) | (b << 32);
}
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) {
    return (a >> 32) | (b & 0xFFFFFFFF00000000ull);
}

/* Packs: a supplies the low half of the result, b the high half. */
static inline uint64_t mmx_packsswb(uint64_t a_, uint64_t b_) {
    RecompMmx a, b, r; int i; a.q = a_; b.q = b_;
    for (i = 0; i < 4; i++) { r.sb[i] = mmx_sat8(a.sw[i]); r.sb[4 + i] = mmx_sat8(b.sw[i]); }
    return r.q;
}
static inline uint64_t mmx_packuswb(uint64_t a_, uint64_t b_) {
    RecompMmx a, b, r; int i; a.q = a_; b.q = b_;
    for (i = 0; i < 4; i++) { r.b[i] = mmx_usat8(a.sw[i]); r.b[4 + i] = mmx_usat8(b.sw[i]); }
    return r.q;
}
static inline uint64_t mmx_packssdw(uint64_t a_, uint64_t b_) {
    RecompMmx a, b, r; a.q = a_; b.q = b_;
    r.sw[0] = mmx_sat16(a.sd[0]); r.sw[1] = mmx_sat16(a.sd[1]);
    r.sw[2] = mmx_sat16(b.sd[0]); r.sw[3] = mmx_sat16(b.sd[1]);
    return r.q;
}

/* Word shuffle / extract / insert, byte mask */
static inline uint64_t mmx_pshufw(uint64_t a_, uint32_t imm) {
    RecompMmx a, r; int i; a.q = a_;
    for (i = 0; i < 4; i++) r.w[i] = a.w[(imm >> (2 * i)) & 3];
    return r.q;
}
static inline uint32_t mmx_pextrw(uint64_t a, uint32_t imm) {
    return (uint32_t)((a >> (16 * (imm & 3))) & 0xFFFFu);
}
static inline uint64_t mmx_pinsrw(uint64_t a, uint32_t v, uint32_t imm) {
    uint32_t sh = 16 * (imm & 3);
    return (a & ~(0xFFFFull << sh)) | ((uint64_t)(v & 0xFFFFu) << sh);
}
static inline uint32_t mmx_pmovmskb(uint64_t a_) {
    RecompMmx a; uint32_t m = 0; int i; a.q = a_;
    for (i = 0; i < 8; i++) m |= (uint32_t)(a.b[i] >> 7) << i;
    return m;
}

#endif /* RECOMP_MMX_H */
