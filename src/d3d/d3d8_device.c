/**
 * D3D8→D3D11 Compatibility Device Implementation
 *
 * Implements the Xbox D3D8 IDirect3DDevice8 interface using D3D11.
 * The game's translated RenderWare code calls D3D8 methods through
 * COM vtables; this layer translates those calls to D3D11 equivalents.
 *
 * Architecture:
 * - D3D11 device and swap chain created during initialization
 * - Render state tracking: D3D8 states mapped to D3D11 state objects
 * - Texture/buffer management: D3D8 resource handles wrap D3D11 resources
 * - Fixed-function pipeline: emulated via D3D11 shaders (the Xbox D3D8
 *   pipeline is configurable but not fully programmable)
 *
 * Build: Requires Windows SDK with d3d11.h and dxgi.h
 */

#include "d3d8_internal.h"
#include <d3d11_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Internal device state
 * ================================================================ */

/* Maximum tracked render states, texture stages, and transforms */
#define MAX_RENDER_STATES    256
#define MAX_TEXTURE_STAGES   4
#define MAX_TSS_STATES       32
#define MAX_TRANSFORMS       512
#define MAX_LIGHTS           8

typedef struct D3D8DeviceState {
    /* D3D11 objects */
    ID3D11Device            *d3d11_device;
    ID3D11DeviceContext     *d3d11_context;
    ID3D11Device            *scanout_device;
    ID3D11DeviceContext     *scanout_context;
    IDXGISwapChain          *swap_chain;

    /* Default render targets */
    ID3D11RenderTargetView  *default_rtv;
    ID3D11DepthStencilView  *default_dsv;
    ID3D11Texture2D         *default_depth;
    ID3D11Texture2D         *render_texture;
    ID3D11Texture2D         *ready_texture;
    ID3D11Texture2D         *readback_texture;
    HANDLE                   scanout_thread;
    volatile LONG            scanout_active;
    volatile LONG            scanout_ready;
    volatile LONG            scanout_generation;
    volatile LONG            scanout_cpu_generation;
    volatile LONG            scanout_visible;
    CRITICAL_SECTION         scanout_cpu_lock;
    BOOL                     scanout_cpu_lock_initialized;
    BYTE                    *scanout_cpu_frame;

    /* Window */
    HWND                    hwnd;
    UINT                    width;
    UINT                    height;
    D3DFORMAT               backbuffer_format;

    /* State tracking */
    DWORD                   render_states[MAX_RENDER_STATES];
    DWORD                   tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    D3DMATRIX               transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8            viewport;
    D3DMATERIAL8            material;
    D3DLIGHT8               lights[MAX_LIGHTS];
    BOOL                    light_enable[MAX_LIGHTS];

    /* Current shader/FVF */
    DWORD                   vertex_shader;
    DWORD                   pixel_shader;

    /* Scene state */
    BOOL                    in_scene;

    /* Reference count */
    LONG                    ref_count;
} D3D8DeviceState;

/* Global device instance (Xbox has a single D3D device) */
static D3D8DeviceState g_device_state;
static IDirect3DDevice8 g_device;
static BOOL g_device_initialized = FALSE;

/* Current resource bindings */
static IDirect3DVertexBuffer8 *g_cur_vb = NULL;
static UINT                    g_cur_vb_stride = 0;
static IDirect3DIndexBuffer8  *g_cur_ib = NULL;
static UINT                    g_cur_ib_base_vertex = 0;
static IDirect3DBaseTexture8  *g_cur_textures[4] = { NULL };

/* Forward declarations */
static const IDirect3DDevice8Vtbl g_device_vtbl;
static void up_ring_shutdown(void);
static BOOL d3d8_capture_backbuffer_bmp(const char *path);

static DWORD WINAPI d3d8_scanout_thread(void *opaque)
{
    D3D8DeviceState *state = (D3D8DeviceState *)opaque;
    LARGE_INTEGER frequency, now;
    LONGLONG next_present, stats_start, period;
    unsigned presented = 0, fresh = 0, repeated = 0;
    unsigned repeat_run = 0, max_repeat_run = 0;
    LONG copied_generation = -1;
    LONG presented_generation = -1;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    next_present = stats_start = now.QuadPart;
    period = frequency.QuadPart / 30;

    while (InterlockedCompareExchange(&state->scanout_active, 0, 0)) {
        ID3D11Texture2D *back_buffer = NULL;
        HRESULT hr = E_FAIL;
        LONG generation;

        QueryPerformanceCounter(&now);
        while (now.QuadPart < next_present &&
               InterlockedCompareExchange(&state->scanout_active, 0, 0)) {
            LONGLONG remaining = next_present - now.QuadPart;
            if (remaining > frequency.QuadPart / 500)
                Sleep(1);
            else
                SwitchToThread();
            QueryPerformanceCounter(&now);
        }
        if (!InterlockedCompareExchange(&state->scanout_active, 0, 0))
            break;

        generation = InterlockedCompareExchange(
            &state->scanout_generation, 0, 0);
        if (InterlockedCompareExchange(&state->scanout_ready, 0, 0) &&
            state->swap_chain && state->ready_texture &&
            SUCCEEDED(IDXGISwapChain_GetBuffer(
                state->swap_chain, 0, &IID_ID3D11Texture2D,
                (void **)&back_buffer))) {
            /* DXGI_SWAP_EFFECT_SEQUENTIAL preserves the backbuffer.  Reuse it
             * while the guest is stalled instead of taking the D3D11
             * immediate-context lock to copy the same texture 30 times/sec. */
            if (generation != copied_generation) {
                if (generation == InterlockedCompareExchange(
                        &state->scanout_cpu_generation, 0, 0) &&
                    state->scanout_cpu_frame) {
                    if (TryEnterCriticalSection(&state->scanout_cpu_lock)) {
                        ID3D11DeviceContext_UpdateSubresource(
                            state->scanout_context,
                            (ID3D11Resource *)state->ready_texture,
                            0, NULL, state->scanout_cpu_frame,
                            state->width * 4u,
                            state->width * state->height * 4u);
                        LeaveCriticalSection(&state->scanout_cpu_lock);
                    } else {
                        /* A producer is still completing this generation.
                         * Present the last complete backbuffer on schedule and
                         * retry the new frame at the next 30 Hz slot. */
                        ID3D11Texture2D_Release(back_buffer);
                        hr = IDXGISwapChain_Present(state->swap_chain, 0, 0);
                        goto scanout_presented;
                    }
                }
                ID3D11DeviceContext_CopyResource(
                    state->scanout_context, (ID3D11Resource *)back_buffer,
                    (ID3D11Resource *)state->ready_texture);
                copied_generation = generation;
            }
            ID3D11Texture2D_Release(back_buffer);
            hr = IDXGISwapChain_Present(state->swap_chain, 0, 0);
scanout_presented:
            if (SUCCEEDED(hr) && presented == 0u) {
                char capture_first[2];
                if (GetEnvironmentVariableA("XBOXRECOMP_CAPTURE_FIRST_SCANOUT",
                                            capture_first,
                                            sizeof(capture_first)))
                    d3d8_capture_backbuffer_bmp(
                        "build-release\\first_scanout.bmp");
            }
        }

        next_present += period;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= next_present) {
            /* Preserve the original 30 Hz phase after a temporary scheduling
             * delay.  Rebasing to now+period compounds one late present into
             * another full-frame pause; advancing by whole periods catches
             * the next real scanout slot without emitting burst frames. */
            const LONGLONG missed =
                (now.QuadPart - next_present) / period + 1;
            next_present += missed * period;
        }
        if (SUCCEEDED(hr)) {
            /* A healthy refresh thread can still repeat a stalled game's
             * last frame. Report publication progress separately; this is
             * not a claim that every submitted frame has different pixels. */
            if (copied_generation != presented_generation) {
                ++fresh;
                repeat_run = 0;
                presented_generation = copied_generation;
            } else {
                ++repeated;
                if (++repeat_run > max_repeat_run)
                    max_repeat_run = repeat_run;
            }
        }
        if (SUCCEEDED(hr) && ++presented == 300u) {
            LONGLONG elapsed = now.QuadPart - stats_start;
            unsigned fps_milli = elapsed > 0
                ? (unsigned)(((uint64_t)presented * 1000u *
                              frequency.QuadPart) / (uint64_t)elapsed)
                : 0u;
            fprintf(stderr,
                    "[SCANOUT-CADENCE] frames=%u fps=%u.%03u fresh=%u "
                    "repeated=%u max-repeat-run=%u\n",
                    presented, fps_milli / 1000u, fps_milli % 1000u,
                    fresh, repeated, max_repeat_run);
            fflush(stderr);
            presented = 0;
            fresh = repeated = 0;
            max_repeat_run = repeat_run;
            stats_start = now.QuadPart;
        }
    }
    return 0;
}

static void d3d8_start_scanout(D3D8DeviceState *state)
{
    if (!state->scanout_thread) {
        InterlockedExchange(&state->scanout_active, 1);
        state->scanout_thread = CreateThread(
            NULL, 0, d3d8_scanout_thread, state, 0, NULL);
        if (!state->scanout_thread)
            InterlockedExchange(&state->scanout_active, 0);
        else
            /* Scanout must remain schedulable while the guest thread parses
             * and uploads a submenu bundle.  It only performs one bounded
             * upload/present per 33.3 ms, so time-critical priority is safe
             * and prevents asset work from starving the display clock. */
            SetThreadPriority(state->scanout_thread,
                              THREAD_PRIORITY_TIME_CRITICAL);
    }
    if (!InterlockedExchange(&state->scanout_visible, 1) && state->hwnd) {
        ShowWindowAsync(state->hwnd, SW_SHOW);
        UpdateWindow(state->hwnd);
    }
}

void d3d8_SubmitRGBAFrame(const void *pixels, UINT row_pitch,
                          UINT slice_pitch)
{
    D3D8DeviceState *state = &g_device_state;
    LONG generation;
    UINT y;
    const BYTE *source = (const BYTE *)pixels;
    (void)slice_pitch;
    if (!pixels || !state->scanout_cpu_frame ||
        !state->scanout_cpu_lock_initialized)
        return;
    /* Rendering and movie decode must never queue behind scanout.  A newer
     * frame arrives on the next tick, so dropping only a contended mailbox
     * write is preferable to stalling either producer and cascading latency. */
    if (!TryEnterCriticalSection(&state->scanout_cpu_lock))
        return;
    for (y = 0; y < state->height; ++y)
        memcpy(state->scanout_cpu_frame + (size_t)y * state->width * 4u,
               source + (size_t)y * row_pitch, state->width * 4u);
    generation = InterlockedIncrement(&state->scanout_generation);
    InterlockedExchange(&state->scanout_cpu_generation, generation);
    LeaveCriticalSection(&state->scanout_cpu_lock);
    InterlockedExchange(&state->scanout_ready, 1);
    d3d8_start_scanout(state);
}

/* Temporary bring-up aid: capture the actual D3D11 backbuffer so projects can
 * verify visual progress even when desktop window capture is unavailable. */
static BOOL d3d8_capture_backbuffer_bmp(const char *path)
{
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER info_header;
    unsigned char *row = NULL;
    FILE *file = NULL;
    ID3D11Device *capture_device = g_device_state.d3d11_device;
    ID3D11DeviceContext *capture_context = g_device_state.d3d11_context;
    HRESULT hr;
    BOOL ok = FALSE;

    if (g_device_state.scanout_ready && g_device_state.ready_texture) {
        back_buffer = g_device_state.ready_texture;
        capture_device = g_device_state.scanout_device;
        capture_context = g_device_state.scanout_context;
        ID3D11Texture2D_AddRef(back_buffer);
        hr = S_OK;
    } else if (g_device_state.render_texture) {
        back_buffer = g_device_state.render_texture;
        ID3D11Texture2D_AddRef(back_buffer);
        hr = S_OK;
    } else {
        hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                      &IID_ID3D11Texture2D,
                                      (void **)&back_buffer);
        capture_device = g_device_state.scanout_device;
        capture_context = g_device_state.scanout_context;
    }
    if (FAILED(hr)) goto cleanup;

    ID3D11Texture2D_GetDesc(back_buffer, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(capture_device, &desc,
                                      NULL, &staging);
    if (FAILED(hr)) goto cleanup;

    ID3D11DeviceContext_CopyResource(capture_context,
                                     (ID3D11Resource *)staging,
                                     (ID3D11Resource *)back_buffer);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(capture_context,
                                 (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) goto cleanup;

    row = (unsigned char *)malloc((size_t)desc.Width * 4u);
    if (!row) {
        ID3D11DeviceContext_Unmap(capture_context,
                                  (ID3D11Resource *)staging, 0);
        goto cleanup;
    }

    file = fopen(path, "wb");
    if (!file) {
        ID3D11DeviceContext_Unmap(capture_context,
                                  (ID3D11Resource *)staging, 0);
        goto cleanup;
    }

    memset(&file_header, 0, sizeof(file_header));
    memset(&info_header, 0, sizeof(info_header));
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits + desc.Width * desc.Height * 4u;
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = (LONG)desc.Width;
    info_header.biHeight = (LONG)desc.Height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = desc.Width * desc.Height * 4u;

    fwrite(&file_header, sizeof(file_header), 1, file);
    fwrite(&info_header, sizeof(info_header), 1, file);
    for (LONG y = (LONG)desc.Height - 1; y >= 0; --y) {
        const unsigned char *src =
            (const unsigned char *)mapped.pData + (size_t)y * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            row[x * 4u + 0u] = src[x * 4u + 2u];
            row[x * 4u + 1u] = src[x * 4u + 1u];
            row[x * 4u + 2u] = src[x * 4u + 0u];
            row[x * 4u + 3u] = 0xFFu;
        }
        fwrite(row, (size_t)desc.Width * 4u, 1, file);
    }
    ok = TRUE;
    ID3D11DeviceContext_Unmap(capture_context,
                              (ID3D11Resource *)staging, 0);

cleanup:
    if (file) fclose(file);
    free(row);
    if (staging) ID3D11Texture2D_Release(staging);
    if (back_buffer) ID3D11Texture2D_Release(back_buffer);
    fprintf(stderr, "D3D8: framebuffer capture %s: %s\n",
            ok ? "written" : "failed", path);
    fflush(stderr);
    return ok;
}

/* Public: save the current backbuffer (debug captures from title code). */
void d3d8_capture_backbuffer_bmp_public(const char *path)
{
    d3d8_capture_backbuffer_bmp(path);
}

/* ================================================================
 * Public frame pump (called from recompiled game code)
 * ================================================================ */
void d3d8_PresentFrame(void)
{
    static DWORD present_count = 0;

    present_count++;
    {
        /* XBOXRECOMP_CAPTURE="n,n,..." overrides the default capture frames. */
        static DWORD frames[32];
        static int frame_count = -1;
        if (frame_count < 0) {
            char spec[256];
            frame_count = 0;
            if (GetEnvironmentVariableA("XBOXRECOMP_CAPTURE", spec, sizeof(spec))) {
                char *p = spec;
                frame_count = 0;
                while (*p && frame_count < 32) {
                    frames[frame_count++] = strtoul(p, &p, 10);
                    while (*p == ',' || *p == ' ') p++;
                }
            }
        }
        for (int i = 0; i < frame_count; ++i) {
            if (present_count == frames[i]) {
                char path[64];
                sprintf_s(path, sizeof(path), "build-release\\frame_%04lu.bmp",
                          (unsigned long)present_count);
                d3d8_capture_backbuffer_bmp(path);
            }
        }
    }

    /* Pump Windows messages */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr,
                    "[HOST-EXIT] d3d8_PresentFrame received WM_QUIT code=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess((UINT)msg.wParam);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Publish a complete frame atomically.  The independent 30 Hz scanout
     * thread repeats this image if guest emulation or asset loading stalls,
     * so host presentation never inherits guest scheduling jitter. */
    if (g_device_state.render_texture && g_device_state.readback_texture &&
        g_device_state.scanout_cpu_frame) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        UINT y;
        LONG generation;
        ID3D11DeviceContext_CopyResource(
            g_device_state.d3d11_context,
            (ID3D11Resource *)g_device_state.readback_texture,
            (ID3D11Resource *)g_device_state.render_texture);
        memset(&mapped, 0, sizeof(mapped));
        if (SUCCEEDED(ID3D11DeviceContext_Map(
                g_device_state.d3d11_context,
                (ID3D11Resource *)g_device_state.readback_texture,
                0, D3D11_MAP_READ, 0, &mapped))) {
            EnterCriticalSection(&g_device_state.scanout_cpu_lock);
            for (y = 0; y < g_device_state.height; ++y)
                memcpy(g_device_state.scanout_cpu_frame +
                           (size_t)y * g_device_state.width * 4u,
                       (const BYTE *)mapped.pData +
                           (size_t)y * mapped.RowPitch,
                       g_device_state.width * 4u);
            generation = InterlockedIncrement(
                &g_device_state.scanout_generation);
            InterlockedExchange(&g_device_state.scanout_cpu_generation,
                                generation);
            LeaveCriticalSection(&g_device_state.scanout_cpu_lock);
            ID3D11DeviceContext_Unmap(
                g_device_state.d3d11_context,
                (ID3D11Resource *)g_device_state.readback_texture, 0);
            InterlockedExchange(&g_device_state.scanout_ready, 1);
            d3d8_start_scanout(&g_device_state);
        }
    }
}

/* ================================================================
 * Internal accessors (used by d3d8_resources/shaders/states)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void) { return &g_device; }
ID3D11Device        *d3d8_GetD3D11Device(void) { return g_device_state.d3d11_device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return g_device_state.d3d11_context; }
IDXGISwapChain      *d3d8_GetSwapChain(void) { return g_device_state.swap_chain; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return g_device_state.default_rtv; }
HWND                 d3d8_GetHWND(void) { return g_device_state.hwnd; }
/* ================================================================
 * Offscreen render targets for the NV2A translator.
 *
 * NV2A titles render into arbitrary RAM surfaces (SET_SURFACE_COLOR_OFFSET)
 * and later sample them as textures; xemu keeps a surface cache for this.
 * Here each non-display surface address gets a D3D11 colour + depth target,
 * and the translator binds it while that surface is current and samples it
 * through d3d8_rt_texture() when a texture points at the same address.
 * "Current target" state (RTV/DSV/size) is what Clear, the viewport and the
 * pre-transformed vertex mapping (ScreenSize) follow.
 * ================================================================ */
typedef struct {
    uint32_t key;
    UINT w, h;
    ID3D11Texture2D *tex, *dtex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11DepthStencilView *dsv;
    IDirect3DTexture8 *wrap;
} D3D8RtEntry;
static D3D8RtEntry g_rts[48];
static ID3D11RenderTargetView *g_cur_rtv;
static ID3D11DepthStencilView *g_cur_dsv;
static UINT g_cur_w, g_cur_h;

static ID3D11RenderTargetView *cur_rtv(void) { return g_cur_rtv ? g_cur_rtv : g_device_state.default_rtv; }
static ID3D11DepthStencilView *cur_dsv(void) { return g_cur_rtv ? g_cur_dsv : g_device_state.default_dsv; }

static void bind_target(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, UINT w, UINT h)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *nulls[4] = { NULL, NULL, NULL, NULL };
    /* a target cannot stay bound as a shader input while drawn into */
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 4, nulls);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1, &rtv, dsv);
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)w; vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
}

static D3D8RtEntry *rt_get(uint32_t key, UINT w, UINT h, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < 48; ++i) {
        if (g_rts[i].key == key && g_rts[i].tex) {
            if (!create || (g_rts[i].w == w && g_rts[i].h == h)) return &g_rts[i];
            free_slot = i;   /* same address, new size: rebuild in place */
            break;
        }
        if (!g_rts[i].tex && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0 || !w || !h) return NULL;
    {
        D3D8RtEntry *e = &g_rts[free_slot];
        D3D11_TEXTURE2D_DESC td;
        ID3D11Device *dev = g_device_state.d3d11_device;
        if (e->rtv) ID3D11RenderTargetView_Release(e->rtv);
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->dsv) ID3D11DepthStencilView_Release(e->dsv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        if (e->dtex) ID3D11Texture2D_Release(e->dtex);
        memset(e, 0, sizeof(*e));
        memset(&td, 0, sizeof(td));
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        /* Match the renderer's publication texture.  Render targets are
         * promoted with CopyResource at a guest frame boundary, which
         * requires an identical DXGI format.  The old BGRA surface could
         * neither be copied to the RGBA publication texture nor directly to
         * the swapchain owned by the separate scanout device. */
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &e->tex))) return NULL;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)e->tex, NULL, &e->rtv);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &e->dtex)))
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)e->dtex, NULL, &e->dsv);
        e->key = key; e->w = w; e->h = h;
        {
            /* expose as an ordinary IDirect3DTexture8 whose view is the RT */
            IDirect3DTexture8 *t = NULL;
            if (SUCCEEDED(d3d8_CreateTextureImpl(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, &t)) && t) {
                D3D8Texture *dt = (D3D8Texture *)t;
                if (dt->srv) ID3D11ShaderResourceView_Release(dt->srv);
                ID3D11ShaderResourceView_AddRef(e->srv);
                dt->srv = e->srv;
                dt->width = w; dt->height = h;
                dt->dirty = FALSE;
                e->wrap = t;
            }
        }
        {
            float black[4] = {0, 0, 0, 0};
            ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context, e->rtv, black);
            if (e->dsv)
                ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context, e->dsv,
                    D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
        return e;
    }
}

/* Make guest surface `key` current (0 = the display back buffer). */
void d3d8_rt_select(uint32_t key, UINT w, UINT h)
{
    if (!key) {
        g_cur_rtv = NULL; g_cur_dsv = NULL; g_cur_w = g_cur_h = 0;
        bind_target(g_device_state.default_rtv, g_device_state.default_dsv,
                    g_device_state.width, g_device_state.height);
        return;
    }
    {
        D3D8RtEntry *e = rt_get(key, w, h, 1);
        if (!e) return;
        g_cur_rtv = e->rtv; g_cur_dsv = e->dsv; g_cur_w = e->w; g_cur_h = e->h;
        bind_target(e->rtv, e->dsv, e->w, e->h);
    }
}

/* Texture view of a guest surface rendered earlier, or NULL. */
IDirect3DTexture8 *d3d8_rt_texture(uint32_t key)
{
    D3D8RtEntry *e = rt_get(key, 0, 0, 0);
    return e ? e->wrap : NULL;
}

/* Copy a completed guest render target to the host display backbuffer.
 *
 * During initial pushbuffer catch-up the translator may encounter several
 * historical colour surfaces before it knows which one ended the frame.  The
 * frame-ending surface is authoritative, but its first rendering may already
 * have gone to an offscreen texture.  Promote that completed image here rather
 * than forcing every intermediate render-to-texture pass onto the display. */
void d3d8_rt_present(uint32_t key)
{
    D3D8RtEntry *e = rt_get(key, 0, 0, 0);
    D3D11_TEXTURE2D_DESC source_desc, target_desc;
    ID3D11DeviceContext *context = g_device_state.d3d11_context;

    if (!e || !e->tex || !g_device_state.render_texture || !context)
        return;

    ID3D11Texture2D_GetDesc(e->tex, &source_desc);
    ID3D11Texture2D_GetDesc(g_device_state.render_texture, &target_desc);
    if (source_desc.Width == target_desc.Width &&
        source_desc.Height == target_desc.Height &&
        source_desc.Format == target_desc.Format) {
        /* Both resources belong to the guest-render device.  PresentFrame
         * subsequently reads render_texture and publishes it atomically to
         * the independently paced scanout device. */
        ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
        ID3D11DeviceContext_CopyResource(context,
                                         (ID3D11Resource *)g_device_state.render_texture,
                                         (ID3D11Resource *)e->tex);
    }
}

UINT                 d3d8_GetBackbufferWidth(void) { return g_cur_rtv ? g_cur_w : g_device_state.width; }
UINT                 d3d8_GetBackbufferHeight(void) { return g_cur_rtv ? g_cur_h : g_device_state.height; }
const DWORD         *d3d8_GetRenderStates(void) { return g_device_state.render_states; }
const DWORD         *d3d8_GetTSS(DWORD stage) { return (stage < MAX_TEXTURE_STAGES) ? g_device_state.tss[stage] : NULL; }
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) {
    return ((DWORD)type < MAX_TRANSFORMS) ? &g_device_state.transforms[(DWORD)type] : NULL;
}

const D3DLIGHT8     *d3d8_GetLight(DWORD index) {
    return (index < MAX_LIGHTS) ? &g_device_state.lights[index] : NULL;
}

BOOL                 d3d8_GetLightEnable(DWORD index) {
    return (index < MAX_LIGHTS) ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8  *d3d8_GetMaterial(void) {
    return &g_device_state.material;
}

UINT                 d3d8_GetNumLights(void) {
    return MAX_LIGHTS;
}

/* ================================================================
 * D3D11 initialization helpers
 * ================================================================ */

static HRESULT d3d11_create_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    UINT create_flags = 0;
    HRESULT hr;

#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = pp->BackBufferCount ? pp->BackBufferCount : 1;
    scd.BufferDesc.Width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    scd.BufferDesc.Height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = pp->hDeviceWindow;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = pp->Windowed;
    /* Preserve the last completed frame so the independent 30 Hz scanout can
     * present it repeatedly while guest emulation loads or transitions. */
    scd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        create_flags,
        NULL, 0,
        D3D11_SDK_VERSION,
        &scd,
        &state->swap_chain,
        &state->scanout_device,
        &feature_level,
        &state->scanout_context
    );

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Failed to create D3D11 device: 0x%08lX\n", hr);
        return hr;
    }

    /* Use a second D3D11 device for guest rendering.  The swapchain device is
     * owned exclusively by the 30 Hz scanout thread, so shader compilation,
     * texture uploads, and synchronous guest readback can never stall host
     * presentation. */
    hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags,
        NULL, 0, D3D11_SDK_VERSION,
        &state->d3d11_device, &feature_level, &state->d3d11_context);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Failed to create render device: 0x%08lX\n",
                hr);
        return hr;
    }

    /* Diagnostic capture can also read the scanout texture from the guest
     * thread. Serialize both immediate contexts: protecting only the render
     * device leaves capture racing scanout's CopyResource/UpdateSubresource. */
    {
        ID3D11DeviceContext *contexts[2] = {
            state->d3d11_context, state->scanout_context
        };
        unsigned i;
        for (i = 0; i < 2; ++i) {
            ID3D11Multithread *multithread = NULL;
            if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(
                    contexts[i], &IID_ID3D11Multithread,
                    (void **)&multithread))) {
                ID3D11Multithread_SetMultithreadProtected(multithread, TRUE);
                ID3D11Multithread_Release(multithread);
            }
        }
    }

    state->hwnd = pp->hDeviceWindow;
    state->width = scd.BufferDesc.Width;
    state->height = scd.BufferDesc.Height;

    return S_OK;
}

static HRESULT d3d11_create_render_targets(D3D8DeviceState *state)
{
    ID3D11Texture2D *back_buffer = NULL;
    D3D11_TEXTURE2D_DESC render_desc;
    D3D11_TEXTURE2D_DESC depth_desc;
    HRESULT hr;

    /* Render offscreen, then atomically publish completed frames to a second
     * texture.  A separately paced thread owns the actual swapchain. */
    hr = IDXGISwapChain_GetBuffer(state->swap_chain, 0,
                                   &IID_ID3D11Texture2D,
                                   (void **)&back_buffer);
    if (FAILED(hr)) return hr;

    ID3D11Texture2D_GetDesc(back_buffer, &render_desc);
    render_desc.Usage = D3D11_USAGE_DEFAULT;
    render_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    render_desc.CPUAccessFlags = 0;
    render_desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &render_desc,
                                      NULL, &state->render_texture);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(back_buffer);
        return hr;
    }

    hr = ID3D11Device_CreateRenderTargetView(state->d3d11_device,
                                              (ID3D11Resource *)state->render_texture,
                                              NULL, &state->default_rtv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(back_buffer);
        return hr;
    }

    render_desc.BindFlags = 0;
    hr = ID3D11Device_CreateTexture2D(state->scanout_device, &render_desc,
                                      NULL, &state->ready_texture);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(back_buffer);
        return hr;
    }

    render_desc.Usage = D3D11_USAGE_STAGING;
    render_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &render_desc,
                                      NULL, &state->readback_texture);
    ID3D11Texture2D_Release(back_buffer);
    if (FAILED(hr)) return hr;

    state->scanout_cpu_frame = (BYTE *)malloc(
        (size_t)state->width * state->height * 4u);
    if (!state->scanout_cpu_frame) return E_OUTOFMEMORY;
    InitializeCriticalSection(&state->scanout_cpu_lock);
    state->scanout_cpu_lock_initialized = TRUE;
    state->scanout_cpu_generation = -1;

    /* Create depth stencil */
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.Width = state->width;
    depth_desc.Height = state->height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &depth_desc,
                                       NULL, &state->default_depth);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateDepthStencilView(state->d3d11_device,
                                              (ID3D11Resource *)state->default_depth,
                                              NULL, &state->default_dsv);
    if (FAILED(hr)) return hr;

    /* Bind default render targets */
    ID3D11DeviceContext_OMSetRenderTargets(state->d3d11_context, 1,
                                            &state->default_rtv,
                                            state->default_dsv);

    return S_OK;
}

static void d3d8_init_default_states(D3D8DeviceState *state)
{
    /* Set Xbox D3D8 default render states */
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE]           = 1;
    state->render_states[D3DRS_FILLMODE]          = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE]         = 2; /* D3DSHADE_GOURAUD */
    state->render_states[D3DRS_ZWRITEENABLE]      = TRUE;
    state->render_states[D3DRS_ALPHATESTENABLE]    = FALSE;
    state->render_states[D3DRS_SRCBLEND]          = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND]         = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE]          = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC]             = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAREF]          = 0;
    state->render_states[D3DRS_ALPHAFUNC]         = D3DCMP_ALWAYS;
    state->render_states[D3DRS_ALPHABLENDENABLE]   = FALSE;
    state->render_states[D3DRS_FOGENABLE]         = FALSE;
    state->render_states[D3DRS_STENCILENABLE]     = FALSE;
    state->render_states[D3DRS_COLORWRITEENABLE]  = 0x0F;

    /* Default viewport */
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->width;
    state->viewport.Height = state->height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;

    /* Identity matrices */
    for (int i = 0; i < MAX_TRANSFORMS; i++) {
        memset(&state->transforms[i], 0, sizeof(D3DMATRIX));
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }

    state->vertex_shader = 0;
    state->pixel_shader = 0;
    state->in_scene = FALSE;
}

/* ================================================================
 * IDirect3DDevice8 method implementations
 * ================================================================ */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    (void)self;
    LONG ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        /* Cleanup subsystems first */
        up_ring_shutdown();
        d3d8_vsh_shutdown();
        d3d8_combiners_shutdown();
        d3d8_states_shutdown();
        d3d8_shaders_shutdown();

        /* Cleanup D3D11 resources */
        D3D8DeviceState *s = &g_device_state;
        InterlockedExchange(&s->scanout_active, 0);
        if (s->scanout_thread) {
            WaitForSingleObject(s->scanout_thread, 2000);
            CloseHandle(s->scanout_thread);
            s->scanout_thread = NULL;
        }
        if (s->scanout_cpu_lock_initialized) {
            DeleteCriticalSection(&s->scanout_cpu_lock);
            s->scanout_cpu_lock_initialized = FALSE;
        }
        free(s->scanout_cpu_frame);
        s->scanout_cpu_frame = NULL;
        if (s->default_dsv) { ID3D11DepthStencilView_Release(s->default_dsv); s->default_dsv = NULL; }
        if (s->default_depth) { ID3D11Texture2D_Release(s->default_depth); s->default_depth = NULL; }
        if (s->default_rtv) { ID3D11RenderTargetView_Release(s->default_rtv); s->default_rtv = NULL; }
        if (s->readback_texture) { ID3D11Texture2D_Release(s->readback_texture); s->readback_texture = NULL; }
        if (s->ready_texture) { ID3D11Texture2D_Release(s->ready_texture); s->ready_texture = NULL; }
        if (s->render_texture) { ID3D11Texture2D_Release(s->render_texture); s->render_texture = NULL; }
        if (s->swap_chain) { IDXGISwapChain_Release(s->swap_chain); s->swap_chain = NULL; }
        if (s->d3d11_context) { ID3D11DeviceContext_Release(s->d3d11_context); s->d3d11_context = NULL; }
        if (s->d3d11_device) { ID3D11Device_Release(s->d3d11_device); s->d3d11_device = NULL; }
        if (s->scanout_context) { ID3D11DeviceContext_Release(s->scanout_context); s->scanout_context = NULL; }
        if (s->scanout_device) { ID3D11Device_Release(s->scanout_device); s->scanout_device = NULL; }
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8)
{
    (void)self; (void)ppD3D8;
    /* TODO: return the factory */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *pCaps)
{
    (void)self; (void)pCaps;
    /* TODO: fill with Xbox NV2A capabilities */
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *pMode)
{
    (void)self; (void)pMode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *pParams)
{
    (void)self; (void)pParams;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP)
{
    (void)self; (void)pPP;
    /* TODO: resize swap chain */
    return S_OK;
}

static DWORD g_d3d_begin_count = 0;
static DWORD g_d3d_end_count = 0;
static DWORD g_d3d_clear_count = 0;
static DWORD g_d3d_draw_count = 0;
static DWORD g_d3d_settransform_count = 0;
static DWORD g_d3d_setrs_count = 0;
static DWORD g_d3d_settexture_count = 0;

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty)
{
    static DWORD frame_count = 0;
    static DWORD last_tick = 0;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)pDirty;

    frame_count++;
    DWORD now = GetTickCount();
    if (last_tick == 0) last_tick = now;
    if (now - last_tick >= 2000) {
        fprintf(stderr, "  [D3D] %.1fs: %u present (%.1f fps), %u begin, %u end, "
                "%u clear, %u draw, %u xform, %u rs, %u tex\n",
                (now - last_tick) / 1000.0, frame_count,
                frame_count * 1000.0 / (now - last_tick),
                g_d3d_begin_count, g_d3d_end_count,
                g_d3d_clear_count, g_d3d_draw_count,
                g_d3d_settransform_count, g_d3d_setrs_count,
                g_d3d_settexture_count);
        fflush(stderr);
        frame_count = 0;
        g_d3d_begin_count = g_d3d_end_count = 0;
        g_d3d_clear_count = g_d3d_draw_count = 0;
        g_d3d_settransform_count = g_d3d_setrs_count = 0;
        g_d3d_settexture_count = 0;
        last_tick = now;
    }

    /* Pump Windows messages: the game's internal main loop drives rendering,
     * so our external message pump never runs. Process messages here to keep
     * the window responsive and handle input. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr,
                    "[HOST-EXIT] dev_Present received WM_QUIT code=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess((UINT)msg.wParam);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return IDXGISwapChain_Present(g_device_state.swap_chain, 1, 0);
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)iBackBuffer; (void)Type; (void)ppSurface;
    /* TODO: wrap back buffer as D3D8 surface */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    g_d3d_begin_count++;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    g_d3d_end_count++;
    return S_OK;
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    (void)self; (void)Count; (void)pRects; (void)Stencil;
    g_d3d_clear_count++;

    if (Flags & D3DCLEAR_TARGET) {
        float clear_color[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,  /* R */
            ((Color >>  8) & 0xFF) / 255.0f,  /* G */
            ((Color >>  0) & 0xFF) / 255.0f,  /* B */
            ((Color >> 24) & 0xFF) / 255.0f,  /* A */
        };
        ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context,
                                                   cur_rtv(), clear_color);
    }

    if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        UINT clear_flags = 0;
        if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D11_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D11_CLEAR_STENCIL;

        if (cur_dsv())
            ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context,
                                                      cur_dsv(), clear_flags, Z, (UINT8)Stencil);
    }

    return S_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
    (void)self;
    g_d3d_settransform_count++;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        g_device_state.transforms[(DWORD)State] = *pMatrix;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    (void)self;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        *pMatrix = g_device_state.transforms[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    g_d3d_setrs_count++;
    if ((DWORD)State < MAX_RENDER_STATES) {
        g_device_state.render_states[(DWORD)State] = Value;
    }
    /* Mark combiner state dirty if any PS register combiner state changed */
    if ((DWORD)State >= D3DRS_PSALPHAINPUTS0 && (DWORD)State <= D3DRS_PSINPUTTEXTURE) {
        d3d8_combiners_mark_dirty();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    (void)self;
    if ((DWORD)State < MAX_RENDER_STATES && pValue) {
        *pValue = g_device_state.render_states[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES) {
        g_device_state.tss[Stage][(DWORD)Type] = Value;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES && pValue) {
        *pValue = g_device_state.tss[Stage][(DWORD)Type];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
    (void)self;
    g_d3d_settexture_count++;
    if (Stage >= 4) return E_INVALIDARG;
    g_cur_textures[Stage] = pTexture;

    /* Bind SRV to pixel shader */
    if (pTexture) {
        D3D8Texture *tex = (D3D8Texture *)pTexture;
        if (tex->srv) {
            ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                Stage, 1, &tex->srv);
        }
        /* Mark texture stage as active */
        if (g_device_state.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
            Stage, 1, &null_srv);
        g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
    (void)self; (void)Stage; (void)ppTexture;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
    (void)self;
    if (StreamNumber != 0) return S_OK; /* Only stream 0 supported */
    g_cur_vb = pStreamData;
    g_cur_vb_stride = Stride;

    if (pStreamData) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)pStreamData;
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &Stride, &offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
    (void)self; (void)StreamNumber; (void)ppStreamData; (void)pStride;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
    (void)self;
    g_cur_ib = pIndexData;
    g_cur_ib_base_vertex = BaseVertexIndex;

    if (pIndexData) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)pIndexData;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32)
            ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
    (void)self; (void)ppIndexData; (void)pBaseVertexIndex;
    return E_NOTIMPL;
}

static D3D11_PRIMITIVE_TOPOLOGY map_primitive_type(D3DPRIMITIVETYPE pt, UINT count, UINT *out_count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: *out_count = count + 2; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_LINELIST:      *out_count = count * 2; return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     *out_count = count + 1; return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_POINTLIST:     *out_count = count;     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_QUADLIST:      *out_count = count * 6; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                  *out_count = 0;          return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* ================================================================
 * Triangle fan / quad list → triangle list conversion
 *
 * D3D11 doesn't support triangle fans or quad lists.
 * Convert vertex data in-place to triangle list.
 * Returns malloc'd buffer (caller must free) or NULL if no conversion needed.
 * ================================================================ */

static void *convert_fan_or_quad(D3DPRIMITIVETYPE pt, const void *src,
                                  UINT prim_count, UINT stride,
                                  UINT *out_vertex_count)
{
    BYTE *dst;
    const BYTE *s = (const BYTE *)src;
    UINT i;

    if (pt == D3DPT_TRIANGLEFAN) {
        /* Fan: vertex 0 is the hub, each triangle is (0, i+1, i+2) */
        UINT tri_verts = prim_count * 3;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            memcpy(dst + (i * 3 + 0) * stride, s, stride);                      /* v0 (hub) */
            memcpy(dst + (i * 3 + 1) * stride, s + (i + 1) * stride, stride);   /* v[i+1] */
            memcpy(dst + (i * 3 + 2) * stride, s + (i + 2) * stride, stride);   /* v[i+2] */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    if (pt == D3DPT_QUADLIST) {
        /* Quad list: each quad (v0,v1,v2,v3) → 2 triangles (v0,v1,v2), (v0,v2,v3) */
        UINT tri_verts = prim_count * 6;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            const BYTE *q = s + i * 4 * stride;
            memcpy(dst + (i * 6 + 0) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 1) * stride, q + 1 * stride, stride);  /* v1 */
            memcpy(dst + (i * 6 + 2) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 3) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 4) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 5) * stride, q + 3 * stride, stride);  /* v3 */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    return NULL; /* no conversion needed */
}

/* ================================================================
 * DrawPrimitiveUP ring buffer
 *
 * Instead of creating and destroying a D3D11 buffer on every
 * DrawPrimitiveUP call, use a persistent ring buffer.
 * ================================================================ */

#define UP_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4MB ring buffer */

static ID3D11Buffer *g_up_ring_buffer = NULL;
static UINT          g_up_ring_offset = 0;

static HRESULT up_ring_init(void)
{
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = UP_RING_BUFFER_SIZE;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &g_up_ring_buffer);
}

static void up_ring_shutdown(void)
{
    if (g_up_ring_buffer) {
        ID3D11Buffer_Release(g_up_ring_buffer);
        g_up_ring_buffer = NULL;
    }
    g_up_ring_offset = 0;
}

/* Upload vertex data to ring buffer, returns offset. Returns (UINT)-1 on failure. */
static UINT up_ring_upload(const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_MAP map_type;
    HRESULT hr;
    UINT offset;

    if (!g_up_ring_buffer) {
        if (FAILED(up_ring_init())) return (UINT)-1;
    }

    if (size > UP_RING_BUFFER_SIZE) return (UINT)-1;

    /* Wrap around if not enough space */
    if (g_up_ring_offset + size > UP_RING_BUFFER_SIZE) {
        g_up_ring_offset = 0;
        map_type = D3D11_MAP_WRITE_DISCARD;
    } else {
        map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
    }

    hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0, map_type, 0, &mapped);
    if (FAILED(hr)) return (UINT)-1;

    offset = g_up_ring_offset;
    memcpy((BYTE *)mapped.pData + offset, data, size);

    ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0);

    g_up_ring_offset = (offset + size + 15) & ~15;  /* 16-byte align */
    return offset;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Prepare pipeline: shaders, input layout, constant buffers, render states */
    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex);
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    (void)self; (void)MinVertexIndex; (void)NumVertices;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT index_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex);
    return S_OK;
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count, vb_size, ring_offset;
    const void *draw_data = pVertexData;
    void *converted = NULL;

    if (!pVertexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Convert triangle fans and quad lists to triangle lists */
    if (PrimitiveType == D3DPT_TRIANGLEFAN || PrimitiveType == D3DPT_QUADLIST) {
        converted = convert_fan_or_quad(PrimitiveType, pVertexData,
                                         PrimitiveCount, VertexStreamZeroStride,
                                         &vertex_count);
        if (converted) draw_data = converted;
    }

    vb_size = vertex_count * VertexStreamZeroStride;

    /* Upload to ring buffer */
    ring_offset = up_ring_upload(draw_data, vb_size);
    if (converted) free(converted);

    if (ring_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind ring buffer at the right offset */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &g_up_ring_buffer, &VertexStreamZeroStride, &ring_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, 0);

    /* Restore previous VB binding if any */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        UINT restore_offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &restore_offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self; (void)MinVertexIndex;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *tmp_vb = NULL, *tmp_ib = NULL;
    UINT index_count, vb_size, ib_size, offset = 0;
    UINT idx_bytes;
    DXGI_FORMAT ib_fmt;
    HRESULT hr;

    if (!pVertexData || !pIndexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    idx_bytes = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
    ib_fmt = (IndexDataFormat == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    vb_size = NumVertices * VertexStreamZeroStride;
    ib_size = index_count * idx_bytes;

    /* Create temp vertex buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = vb_size;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pVertexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_vb);
    if (FAILED(hr)) return hr;

    /* Create temp index buffer */
    bd.ByteWidth = ib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = pIndexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_ib);
    if (FAILED(hr)) { ID3D11Buffer_Release(tmp_vb); return hr; }

    /* Bind, prepare, draw */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &tmp_vb, &VertexStreamZeroStride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
        tmp_ib, ib_fmt, 0);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0);

    /* Cleanup temp buffers */
    ID3D11Buffer_Release(tmp_ib);
    ID3D11Buffer_Release(tmp_vb);

    /* Restore previous bindings */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &offset);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateTextureImpl(Width, Height, Levels, Usage, Format, ppTexture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateVertexBufferImpl(Length, Usage, FVF, ppVertexBuffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateIndexBufferImpl(Length, Usage, Format, ppIndexBuffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)Lockable; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface)
{
    (void)self; (void)pRenderTarget; (void)pZStencilSurface;
    /* TODO: resolve D3D8 surface to D3D11 RTV/DSV */
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget)
{
    (void)self; (void)ppRenderTarget;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface)
{
    (void)self; (void)ppZStencilSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) {
        g_device_state.viewport = *pViewport;

        D3D11_VIEWPORT d3d11_vp;
        d3d11_vp.TopLeftX = (FLOAT)pViewport->X;
        d3d11_vp.TopLeftY = (FLOAT)pViewport->Y;
        d3d11_vp.Width    = (FLOAT)pViewport->Width;
        d3d11_vp.Height   = (FLOAT)pViewport->Height;
        d3d11_vp.MinDepth = pViewport->MinZ;
        d3d11_vp.MaxDepth = pViewport->MaxZ;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &d3d11_vp);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) *pViewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) g_device_state.material = *pMaterial;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) *pMaterial = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) g_device_state.lights[Index] = *pLight;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) *pLight = g_device_state.lights[Index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD Index, BOOL Enable)
{
    (void)self;
    if (Index < MAX_LIGHTS) g_device_state.light_enable[Index] = Enable;
    return S_OK;
}

static HRESULT __stdcall dev_CreateVertexShader(IDirect3DDevice8 *self, const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
    (void)self; (void)pDeclaration; (void)Usage;
    if (!pHandle) return E_INVALIDARG;
    if (!pFunction) return E_INVALIDARG;
    /* Count instructions: each is 4 DWORDs, last has bit 0 of word[3] set (END flag) */
    {
        int i, num_insns = 0;
        for (i = 0; i < 136; i++) {
            num_insns++;
            if (pFunction[i * 4 + 3] & 1) break;  /* END bit in last word */
        }
        return d3d8_vsh_create_shader(pFunction, num_insns, pHandle);
    }
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.vertex_shader = Handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self;
    d3d8_vsh_set_constant(Register, pConstantData, ConstantCount);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.pixel_shader = Handle;
    d3d8_combiners_set_pixel_shader(Handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self; (void)Register; (void)pConstantData; (void)ConstantCount;
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    (void)self; (void)Flags; (void)pRamp;
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp)
{
    (void)self; (void)pRamp;
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries)
{
    (void)self; (void)PaletteNumber; (void)pEntries;
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush)
{
    (void)self; (void)Count; (void)ppPush;
    /* TODO: Xbox push buffer emulation */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *pPush)
{
    (void)self; (void)pPush;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD Flags)
{
    (void)self; (void)Flags;

    /* Pump Windows messages (same as dev_Present) */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr,
                    "[HOST-EXIT] dev_Swap received WM_QUIT code=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess((UINT)msg.wParam);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return IDXGISwapChain_Present(g_device_state.swap_chain, 1, 0);
}

/* ================================================================
 * Vtable
 * ================================================================ */

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
};

/* ================================================================
 * Public API
 * ================================================================ */

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

/* ================================================================
 * IDirect3D8 factory implementation
 * ================================================================ */

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref = 0;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice)
{
    (void)self; (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
    HRESULT hr;

    if (!pPP || !ppDevice) return E_INVALIDARG;

    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;

    if (!pPP->hDeviceWindow) pPP->hDeviceWindow = hFocusWindow;

    hr = d3d11_create_device_and_swap_chain(&g_device_state, pPP);
    if (FAILED(hr)) return hr;

    hr = d3d11_create_render_targets(&g_device_state);
    if (FAILED(hr)) return hr;

    d3d8_init_default_states(&g_device_state);

    /* Set initial viewport (D3D11 requires explicit viewport) */
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = (FLOAT)g_device_state.width;
        vp.Height   = (FLOAT)g_device_state.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
    }

    /* Initialize shader and state subsystems */
    hr = d3d8_shaders_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Shader init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_states_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: State init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_combiners_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Combiner init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to fixed-function pixel shaders */
    }

    hr = d3d8_vsh_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VSH init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to FVF vertex shaders */
    }

    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;

    *ppDevice = &g_device;
    fprintf(stderr, "D3D8: Device created (%ux%u)\n", g_device_state.width, g_device_state.height);
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface,
    d3d8_AddRef,
    d3d8_Release,
    d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}
