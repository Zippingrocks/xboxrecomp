/*
 * MCPX APU GP/EP DSPs
 *
 * Port of xemu's hw/xbox/mcpx/apu/dsp/gp_ep.c onto the DSP56300 interpreter
 * in dsp/. The Global Processor runs the title's effects image (DirectSound
 * loads dsstdfx.bin into it and talks to it through a command mailbox in
 * system RAM), the Encode Processor produces the final output.
 *
 * Differences from xemu: GCC case ranges are written as range checks (MSVC),
 * there is no settings UI (the full pipeline is always on, interpreter only),
 * and when neither DSP is running the VP mixbins are passed through to the
 * monitor as the previous stub did, so audio works before a title starts
 * its DSPs.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

static const int16_t ep_silence[256][2] = { 0 };

#define IN_RANGE(a, base, count) ((a) >= (base) && (a) < (base) + (count) * 4)

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    static int last_known_dsp_pref = -1;

    if (last_known_dsp_pref != (int)g_config.audio.use_dsp) {
        if (g_config.audio.use_dsp) {
            d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
            d->gp.realtime = true;
            d->ep.realtime = true;
        } else {
            d->monitor.point = MCPX_APU_DEBUG_MON_VP;
            d->gp.realtime = false;
            d->ep.realtime = false;
        }
        last_known_dsp_pref = g_config.audio.use_dsp;
    }
}

static void scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    while (len > 0) {
        if (page_entry > max_sge)
            return;

        uint32_t prd_address = ldl_le_phys(&address_space_memory,
                                           sge_base + page_entry * 8 + 0);
        hwaddr paddr = (prd_address + offset_in_page) & 0x07FFFFFFu;

        if (bytes_to_copy > len) {
            bytes_to_copy = (unsigned int)len;
        }

        if (dir) {
            memcpy(&d->ram_ptr[paddr], ptr, bytes_to_copy);
        } else {
            memcpy(ptr, &d->ram_ptr[paddr], bytes_to_copy);
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = TARGET_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = (unsigned int)len;
        }
        if (!(cur >= base && cur + bytes_to_copy <= end) || !bytes_to_copy)
            return cur;
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        if (index >= GP_OUTPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        if (index >= GP_INPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* DSP hangs if current >= end; but forces current >= base */
    if (cur >= end) return;
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_AC97) {
        return false;
    } else if ((d->monitor.point == MCPX_APU_DEBUG_MON_EP) ||
               (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP)) {
        size_t n = len < sizeof(d->monitor.frame_buf) ? len : sizeof(d->monitor.frame_buf);
        memcpy(d->monitor.frame_buf, ptr, n);
    }

    return true;
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        if (index >= EP_OUTPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        if (index >= EP_INPUT_FIFO_COUNT) return;
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    if (dir && index == 0) {
        bool did_sink = ep_sink_samples(d, ptr, len);
        if (did_sink && len <= sizeof(ep_silence)) {
            /* Since we are sinking, push silence out */
            ptr = (uint8_t *)ep_silence;
        }
    }

    /* DSP hangs if current >= end; but forces current >= base */
    if (end > base && cur >= end) {
        cur = cur % (end - base);
    }
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP. addr is the offset within the GP
 * window (APU base + 0x30000). */
uint64_t mcpx_apu_gp_read(MCPXAPUState *d, hwaddr addr)
{
    uint64_t r;
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        r = dsp_read_memory(d->gp.dsp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        r = dsp_read_memory(d->gp.dsp, 'X',
                            GP_DSP_MIXBUF_BASE + (uint32_t)(addr - NV_PAPU_GPMIXBUF) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        r = dsp_read_memory(d->gp.dsp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        r = dsp_read_memory(d->gp.dsp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4);
    } else {
        r = d->gp.regs[addr & 0xFFFF];
    }
    return r;
}

void mcpx_apu_gp_write(MCPXAPUState *d, hwaddr addr, uint64_t val)
{
    qemu_mutex_lock(&d->lock);
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        dsp_write_memory(d->gp.dsp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        dsp_write_memory(d->gp.dsp, 'X',
                         GP_DSP_MIXBUF_BASE + (uint32_t)(addr - NV_PAPU_GPMIXBUF) / 4,
                         (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        dsp_write_memory(d->gp.dsp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        dsp_write_memory(d->gp.dsp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4, (uint32_t)val);
    } else if (addr == NV_PAPU_GPRST) {
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], (uint32_t)val);
        d->gp.regs[NV_PAPU_GPRST] = (uint32_t)val;
    } else {
        d->gp.regs[addr & 0xFFFF] = (uint32_t)val;
    }
    qemu_mutex_unlock(&d->lock);
}

/* Encode Processor - encoding DSP. addr is the offset within the EP window
 * (APU base + 0x50000). */
uint64_t mcpx_apu_ep_read(MCPXAPUState *d, hwaddr addr)
{
    uint64_t r;
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        r = dsp_read_memory(d->ep.dsp, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        r = dsp_read_memory(d->ep.dsp, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        r = dsp_read_memory(d->ep.dsp, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4);
    } else {
        r = d->ep.regs[addr & 0xFFFF];
    }
    return r;
}

void mcpx_apu_ep_write(MCPXAPUState *d, hwaddr addr, uint64_t val)
{
    qemu_mutex_lock(&d->lock);
    addr &= ~(hwaddr)3;
    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        dsp_write_memory(d->ep.dsp, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        dsp_write_memory(d->ep.dsp, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        dsp_write_memory(d->ep.dsp, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4, (uint32_t)val);
    } else if (addr == NV_PAPU_EPRST) {
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], (uint32_t)val);
        d->ep.regs[NV_PAPU_EPRST] = (uint32_t)val;
        d->ep_frame_div = 0; /* FIXME (xemu): still unsure about frame sync */
    } else {
        d->ep.regs[addr & 0xFFFF] = (uint32_t)val;
    }
    qemu_mutex_unlock(&d->lock);
}

static bool proc_running(const uint32_t *regs, hwaddr rst)
{
    return (regs[rst] & NV_PAPU_GPRST_GPRST) && (regs[rst] & NV_PAPU_GPRST_GPDSPRST);
}

void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    bool gp_enabled = proc_running(d->gp.regs, NV_PAPU_GPRST);
    bool ep_enabled = proc_running(d->ep.regs, NV_PAPU_EPRST);

    /* Write VP results to the GP DSP MIXBUF */
    for (int mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
        uint32_t base = GP_DSP_MIXBUF_BASE + mixbin * NUM_SAMPLES_PER_FRAME;
        for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            dsp_write_memory(d->gp.dsp, 'X', base + sample,
                             float_to_24b(mixbins[mixbin][sample]));
        }
    }

    /* Run GP */
    if (gp_enabled) {
        int budget = 2000; /* bound a runaway program to ~2M cycles a frame */
        dsp_start_frame(d->gp.dsp);
        dsp_set_halt_requested(d->gp.dsp, false);
        dsp_set_cycle_count(d->gp.dsp, 0);
        do {
            dsp_run(d->gp.dsp, 1000);
        } while (!dsp_get_halt_requested(d->gp.dsp) && d->gp.realtime && --budget > 0);
        g_dbg.gp.cycles = dsp_get_cycle_count(d->gp.dsp);

        if ((d->monitor.point == MCPX_APU_DEBUG_MON_GP) ||
            (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP && !ep_enabled)) {
            int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                uint32_t l = dsp_read_memory(d->gp.dsp, 'X', 0x1400 + i);
                d->monitor.frame_buf[off + i][0] = (int16_t)(l >> 8);
                uint32_t r = dsp_read_memory(d->gp.dsp, 'X', 0x1400 + 1 * 0x20 + i);
                d->monitor.frame_buf[off + i][1] = (int16_t)(r >> 8);
            }
        }
    }

    /* Run EP */
    if (ep_enabled && d->ep_frame_div % 8 == 0) {
        int budget = 2000;
        dsp_start_frame(d->ep.dsp);
        dsp_set_halt_requested(d->ep.dsp, false);
        dsp_set_cycle_count(d->ep.dsp, 0);
        do {
            dsp_run(d->ep.dsp, 1000);
        } while (!dsp_get_halt_requested(d->ep.dsp) && d->ep.realtime && --budget > 0);
        g_dbg.ep.cycles = dsp_get_cycle_count(d->ep.dsp);
    }

    /* Neither DSP running yet: pass the front mixbins straight through. */
    if (!gp_enabled && !ep_enabled && d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left = mixbins[0][i], right = mixbins[1][i];
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw, true);
    dsp_set_halt_requested(d->gp.dsp, false);
    dsp_set_cycle_count(d->gp.dsp, 0);

    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw, false);
    dsp_set_halt_requested(d->ep.dsp, false);
    dsp_set_cycle_count(d->ep.dsp, 0);

    mcpx_apu_update_dsp_preference(d);
    fprintf(stderr, "[APU] DSP GP/EP initialized (xemu DSP56300 interpreter)\n");
}
