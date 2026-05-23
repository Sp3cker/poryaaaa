#include "m4a_driver.h"
#include "m4a_internal.h"

#include <stdlib.h>
#include <string.h>

void m4a_internal_recompute_vblank_step(M4ADriver *drv) {
    if (drv->host_rate > 0.0f)
        drv->vblank_step = (double)drv->host_rate / (double)M4A_VBLANK_HZ;
    else
        drv->vblank_step = 0.0;
}

static uint32_t soundcnt_h_from_stereo(bool stereo) {
    if (stereo)
        return 0x210Eu; /* DMA A->R, DMA B->L, A/B/CGB full */
    return 0x3302u;     /* DMA A/B->both, A/B half, CGB full */
}

static void apply_soundcnt_h(M4ADriver *drv, uint32_t value) {
    drv->regs.psg_volume_code     = (uint8_t)(value & 0x03);
    drv->regs.dma_a_volume_code   = (uint8_t)((value >> 2) & 0x01);
    drv->regs.dma_b_volume_code   = (uint8_t)((value >> 3) & 0x01);
    drv->regs.dma_a_enable_right  = (value & (1u << 8))  != 0;
    drv->regs.dma_a_enable_left   = (value & (1u << 9))  != 0;
    drv->regs.dma_b_enable_right  = (value & (1u << 12)) != 0;
    drv->regs.dma_b_enable_left   = (value & (1u << 13)) != 0;
}

M4ADriver *m4a_driver_create(float host_sample_rate) {
    M4ADriver *drv = (M4ADriver *)calloc(1, sizeof(*drv));
    if (!drv) return NULL;
    drv->host_rate = host_sample_rate;
    drv->song_volume = 127;
    drv->master_volume = 15;
    drv->tempo_bpm = 120.0;

    /* Register-file defaults match what real m4a writes during init —
     * NR50/NR51/SOUNDCNT_H/SOUNDBIAS — so a chip reading the snapshot
     * before any vblank doesn't see globally-muted defaults.  See
     * pokeemerald m4a.c m4a_init for the canonical writes. */
    drv->regs.psg_master_enabled  = true;
    drv->regs.master_vol_left     = 7;        /* NR50 high nibble: max */
    drv->regs.master_vol_right    = 7;        /* NR50 low nibble:  max */
    /* Pokemon Emerald m4a_init writes DMA A -> right, DMA B -> left,
     * with full A/B/CGB mix. */
    apply_soundcnt_h(drv, soundcnt_h_from_stereo(true));
    drv->regs.bias_level          = 0x200;    /* SOUNDBIAS hardware default */
    drv->regs.bias_sampling_cycle = 0;        /* 32768 Hz quirk rate */

    /* m4a tempo defaults: ply_tempo defaults to 75, and the doubling step
     * (D = ply_tempo*2) is canonical from MPlayMain. */
    drv->tempoD = 150;
    drv->tempoU = 0x100;
    drv->tempoI = drv->tempoD;
    drv->tempoC = 0;

    /* All four CGB channel `type` slots populated; chip indexing matches the
     * register-file layout (sq1, sq2, wave, noise).  panMask = 0xFF means
     * "no per-channel routing restriction" — m4a_chn_vol_set_cgb's
     * ch->pan &= ch->panMask must not zero pan when the cgb_pan() result
     * is 0xFF (centered) or 0x0F/0xF0 (hard-panned). */
    for (int i = 0; i < M4A_MAX_CGB_CHANNELS; i++) {
        drv->cgb[i].type = (uint8_t)(i + 1);
        drv->cgb[i].trackIndex = -1;
        drv->cgb[i].panMask = 0xFF;
    }

    /* Default per-track init: pan center, bend range 2 semitones, vol 100. */
    for (int i = 0; i < M4A_MAX_TRACKS; i++) {
        drv->tracks[i].rawVolume = 100;
        drv->tracks[i].volume = 100;
        drv->tracks[i].volX = 64;
        drv->tracks[i].pan = 0;
        drv->tracks[i].bendRange = 2;
        drv->tracks[i].lfoSpeed = 22;
    }

    m4a_internal_recompute_vblank_step(drv);
    return drv;
}

void m4a_driver_destroy(M4ADriver *drv) {
    free(drv);
}

void m4a_driver_set_host_rate(M4ADriver *drv, float hz) {
    if (!drv) return;
    drv->host_rate = hz;
    m4a_internal_recompute_vblank_step(drv);
}

void m4a_driver_set_xcmd_callback(M4ADriver *drv,
                                  M4ADriverXcmdFn fn, void *ctx) {
    if (!drv) return;
    drv->xcmd_fn = fn;
    drv->xcmd_ctx = ctx;
}

void m4a_driver_set_voicegroup(M4ADriver *drv, ToneData *vg) {
    if (!drv) return;
    drv->voicegroup = vg;
}

void m4a_driver_refresh_voices(M4ADriver *drv) {
    if (!drv || !drv->voicegroup) return;
    /* Mirror v1 m4a_engine_refresh_voices: re-copy each track's voice from
     * the (possibly edited) voicegroup so currentVoice picks up GUI-side
     * tweaks for already-programmed tracks.  No channel state changes —
     * the next note_on uses the refreshed voice. */
    for (int i = 0; i < M4A_MAX_TRACKS; i++)
        drv->tracks[i].currentVoice = drv->voicegroup[drv->tracks[i].currentProgram];
}

/* m4a_set_song_volume lives in m4a_track.c: it has to recompute every
 * track's effective volume and refresh active CGB channel volumes, which
 * is most of the track-side surface area. */

void m4a_set_master_volume(M4ADriver *drv, uint8_t volume) {
    if (!drv) return;
    drv->master_volume = volume;
}

extern void m4a_chn_vol_set_cgb(M4ADriverCgbChan *ch, bool mono);

void m4a_set_stereo(M4ADriver *drv, bool stereo) {
    if (!drv) return;

    drv->mono_output = !stereo;
    uint32_t soundcnt_h = soundcnt_h_from_stereo(stereo);
    apply_soundcnt_h(drv, soundcnt_h);
    m4a_internal_emit_event(drv, M4A_REG_SOUNDCNT_H, soundcnt_h);

    for (int i = 0; i < M4A_MAX_CGB_CHANNELS; i++) {
        M4ADriverCgbChan *ch = &drv->cgb[i];
        if (!(ch->status & M4A_CHN_ON)) continue;
        m4a_chn_vol_set_cgb(ch, drv->mono_output);
        ch->modify |= M4A_MO_VOL;
    }
}

void m4a_set_reverb_amount(M4ADriver *drv, uint8_t amount) {
    if (!drv) return;
    drv->reverb_amount = amount;
}

void m4a_set_analog_filter(M4ADriver *drv, bool enabled) {
    if (!drv) return;
    drv->analog_filter = enabled;
}

void m4a_set_max_pcm_channels(M4ADriver *drv, uint8_t maxChannels) {
    if (!drv) return;
    drv->max_pcm_channels = maxChannels;
}

void m4a_set_tempo_bpm(M4ADriver *drv, double bpm) {
    if (!drv) return;
    drv->tempo_bpm = bpm;
    /* MPlayMain: tempoI = (tempoD * tempoU) >> 8.  We map bpm → tempoU as
     * (bpm/120)*256, keeping tempoD at its 150 default. */
    if (bpm > 0.0) {
        double scale = bpm / 120.0;
        if (scale < 0.0) scale = 0.0;
        if (scale > 16.0) scale = 16.0;
        drv->tempoU = (uint16_t)(scale * 256.0 + 0.5);
        drv->tempoI = (uint16_t)(((uint32_t)drv->tempoD * drv->tempoU) >> 8);
    }
}

const M4ARegisterFile *m4a_get_register_file(const M4ADriver *drv) {
    return drv ? &drv->regs : NULL;
}

M4ARegisterFile *m4a_get_register_file_mut(M4ADriver *drv) {
    return drv ? &drv->regs : NULL;
}

const M4APcmRing *m4a_get_pcm_ring(const M4ADriver *drv) {
    return drv ? &drv->pcm : NULL;
}

/* ---- Layer 1.5 event-queue accessors / helpers ---- */

void m4a_internal_emit_event(M4ADriver *drv, M4ARegId reg, uint32_t value) {
    if (!drv) return;
    if (drv->event_count >= M4A_EVENT_QUEUE_CAP) {
        drv->events_dropped++;
        return;
    }
    M4ARegWrite *ev = &drv->events[drv->event_count++];
    ev->sample_offset = drv->event_vblank_offset;
    ev->reg           = reg;
    ev->value         = value;
}

const M4ARegWriteBatch *m4a_get_pending_writes(const M4ADriver *drv) {
    if (!drv) return NULL;
    /* The batch struct lives inside M4ADriver; we rebuild its view here
     * so it always reflects the current event_count without forcing
     * every emit_event to update it. */
    M4ARegWriteBatch *batch = (M4ARegWriteBatch *)&drv->event_batch;
    batch->events = drv->events;
    batch->count  = drv->event_count;
    return batch;
}

uint32_t m4a_get_events_dropped(const M4ADriver *drv) {
    return drv ? drv->events_dropped : 0;
}

void m4a_consume_writes(M4ADriver *drv) {
    if (!drv) return;
    drv->event_count          = 0;
    drv->event_render_offset  = 0;
    drv->event_vblank_offset  = 0;

    /* Snapshot trigger latches are conceptually a side-effect view of
     * the same NRx4-with-trigger event the queue carried.  Once the
     * chip has consumed the batch, the latches must reset — otherwise
     * any later snapshot consumer (UI, debug, hw_audio_render fallback)
     * would see ghost triggers.  Mirrors §6a "Edge-trigger latches.
     * Driver sets ... Chip clears after consuming." */
    drv->regs.trigger_sq1   = false;
    drv->regs.trigger_sq2   = false;
    drv->regs.trigger_wave  = false;
    drv->regs.trigger_noise = false;
}
