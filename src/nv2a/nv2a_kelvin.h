/*
 * NV2A Kelvin (NV097) pushbuffer -> D3D8 layer translator.
 *
 * A generic replacement for the menu-capture translator in
 * nv2a_pgraph_d3d11.c: vertex programs are interpreted on the CPU, fixed
 * function position transform uses the composite matrix, vertex data comes
 * from inline arrays or guest memory arrays, and textures are decoded from
 * guest memory. Output is drawn as pre-transformed (XYZRHW) geometry through
 * the D3D8-on-D3D11 device.
 */
#ifndef NV2A_KELVIN_H
#define NV2A_KELVIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void kelvin_init(void);
/* Handle one NV097 method. Returns 1 when the method was understood. */
int kelvin_method(uint32_t subchannel, uint32_t method, uint32_t param);
/* End of frame: flush pending work and age the texture cache. */
void kelvin_end_frame(void);

typedef struct KelvinStats {
    uint32_t draws;
    uint32_t vertices;
    uint32_t program_draws;
    uint32_t ff_draws;
    uint32_t textures_decoded;
    uint32_t unsupported_texture_formats;
    uint32_t clears;
} KelvinStats;

void kelvin_get_stats(KelvinStats *out);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_KELVIN_H */
