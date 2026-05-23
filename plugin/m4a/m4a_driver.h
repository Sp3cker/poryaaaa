#ifndef M4A_DRIVER_H
#define M4A_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

#include "m4a_register_file.h"
#include "m4a_pcm_ring.h"
#include "voicegroup/voicegroup_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Software m4a driver.  Mirrors the real m4a routines from the
 * pokeemerald disassembly (Music 4 Advance / Sappy / MPlayDef).
 *
 * Status (2026-04-29): MIDI ingress drives real CGB + DirectSound
 * channel state; m4a_advance fires SoundMain at vblank cadence
 * (CgbSound + SoundMainRAM + SoundMainRAM_Reverb + LFO tick).  The
 * register-file (M4ARegisterFile) and PCM ring (M4APcmRing) are the
 * authoritative driver→chip outputs.  Driver side is now feature-
 * complete for the parity targets we care about — §12 step 1's LFO
 * advancement landed (m4a_internal_lfo_tick in m4a_track.c, fired
 * from m4a_main.c's tempoC overflow loop); MPlayMain song-walk
 * remains a deliberate no-op because the DAW is the song player.
 * §12 step 2 PCM mixer + reverb still needs a verification audit
 * against v1 and real m4a before this output is a valid parity
 * baseline.  Audibility is achieved end-to-end behind v2 flags;
 * mGBA capture-comparison parity (§12 step 10b) remains the open
 * parity gate — see plan §12 blocking-gates list. */
typedef struct M4ADriver M4ADriver;

/* Mirror of M4AEngineXcmdFn so v2 can be wired into poryaaaa's xcmd path
 * without dragging in m4a_engine.h. */
typedef void (*M4ADriverXcmdFn)(void *ctx, int trackIndex,
                                uint8_t selector, uint32_t value);

/* ---- Layer 1.5 event-stream contract (HW_AUDIO_SCAFFOLD_PLAN.md §6c) ----
 *
 * Authoritative driver→chip interface from Layer 1.5 onwards.  Driver
 * appends register-write events in CgbSound's actual emit order; chip
 * segments its render span at each event's sample_offset and applies
 * the write at that boundary, exactly as mGBA does with
 * GBAAudioSample() + write.
 *
 * The snapshot (M4ARegisterFile) is still computed as a side effect and
 * remains queryable via m4a_get_register_file() for non-timing
 * consumers (UI, params, debug).  Chip-internal logic from this layer
 * onwards MUST drive off the event stream — using the snapshot for
 * timing-sensitive PSG decisions defeats the whole point. */
typedef enum {
    M4A_REG_NR10, M4A_REG_NR11, M4A_REG_NR12, M4A_REG_NR13, M4A_REG_NR14,
    M4A_REG_NR21, M4A_REG_NR22, M4A_REG_NR23, M4A_REG_NR24,
    M4A_REG_NR30, M4A_REG_NR31, M4A_REG_NR32, M4A_REG_NR33, M4A_REG_NR34,
    M4A_REG_NR41, M4A_REG_NR42, M4A_REG_NR43, M4A_REG_NR44,
    M4A_REG_NR50, M4A_REG_NR51, M4A_REG_NR52,
    M4A_REG_SOUNDCNT_H, M4A_REG_SOUNDBIAS,
    /* Per §6c: wave RAM is byte-granular (16 events for a full rewrite,
     * matching m4a's STMIA write order).  value = (addr_in_wave_ram << 8) | byte. */
    M4A_REG_WAVE_RAM_BYTE,
    /* PCM ring publish gate.  Driver emits one per vblank at
     * sample_offset = vblank firing offset.  Chip-side `hw_pcm`
     * advances `pcm_published_through` by M4A_PCM_SAMPLES_PER_VBLANK
     * on each event; reads from the ring are clamped to the published
     * range so pre-vblank host samples can't see ring data that
     * later vblanks within the same render call wrote.  No payload —
     * the increment is constant.  See HW_AUDIO_SCAFFOLD_PLAN.md
     * "DirectSound PCM event/ring timing" blocking gate. */
    M4A_REG_PCM_PUBLISH,
} M4ARegId;

typedef struct {
    uint32_t sample_offset;   /* 0..frames-1 within the current render span */
    M4ARegId reg;
    uint32_t value;           /* register payload, see plan §6c */
} M4ARegWrite;

typedef struct {
    const M4ARegWrite *events;
    size_t             count;
} M4ARegWriteBatch;

/* Lifecycle */
M4ADriver *m4a_driver_create(float host_sample_rate);
void       m4a_driver_destroy(M4ADriver *drv);
void       m4a_driver_set_host_rate(M4ADriver *drv, float hz);
void       m4a_driver_set_xcmd_callback(M4ADriver *drv,
                                        M4ADriverXcmdFn fn, void *ctx);

/* Voicegroup wiring (driver receives an already-populated ToneData*). */
void m4a_driver_set_voicegroup(M4ADriver *drv, ToneData *vg);
void m4a_driver_refresh_voices(M4ADriver *drv);

/* MIDI ingress.  Signatures mirror the v1 engine surface so the v2
 * mirror call at each ingress site is mechanical. */
void m4a_note_on(M4ADriver *drv, int track, uint8_t key, uint8_t velocity);
void m4a_note_off(M4ADriver *drv, int track, uint8_t key);
void m4a_cc(M4ADriver *drv, int track, uint8_t cc, uint8_t value);
void m4a_pitch_bend(M4ADriver *drv, int track, int16_t bend);
void m4a_program_change(M4ADriver *drv, int track, uint8_t program);
void m4a_all_notes_off(M4ADriver *drv, int track);
void m4a_all_sound_off(M4ADriver *drv);

/* Engine-level params.  Mirror the same surface plugin/standalone uses
 * to push host/GUI config into the v1 engine. */
void m4a_set_song_volume(M4ADriver *drv, uint8_t volume);
void m4a_set_master_volume(M4ADriver *drv, uint8_t volume);   /* 0..15 m4a master */
void m4a_set_reverb_amount(M4ADriver *drv, uint8_t amount);   /* 0..127 */
void m4a_set_analog_filter(M4ADriver *drv, bool enabled);     /* chip-side LPF */
void m4a_set_max_pcm_channels(M4ADriver *drv, uint8_t maxChannels);
void m4a_set_stereo(M4ADriver *drv, bool stereo);
void m4a_set_tempo_bpm(M4ADriver *drv, double bpm);

/* Advance the driver's internal vblank clock by `host_frames` at the
 * configured host rate.  Each elapsed vblank fires SoundMain
 * internally:
 *   - c15 cycle + tempo accumulator
 *   - CgbSound: ticks each CGB channel's envelope, writes to the
 *     M4ARegisterFile snapshot, and queues NRxx events with sample_offset
 *     stamps for the chip's event-driven render path.
 *   - SoundMainRAM: PCM voice mix → reverb → clamp → write into the
 *     M4APcmRing at write_cursor.
 *
 * Bound `host_frames` to M4A_RECOMMENDED_MAX_ADVANCE_FRAMES per
 * render-event-consume cycle (chunk if larger).  m4a_get_events_dropped()
 * reports any overflow. */
void m4a_advance(M4ADriver *drv, int host_frames);

/* Read-only accessor for non-timing consumers (UI, params, debug). */
const M4ARegisterFile *m4a_get_register_file(const M4ADriver *drv);
const M4APcmRing      *m4a_get_pcm_ring(const M4ADriver *drv);

/* Mutable accessor for the chip's render path.  The chip *consumes*
 * edge-trigger latches (trigger_sq1/sq2/wave/noise) by clearing them
 * after applying the corresponding NRx4 write — see §6a "Edge-trigger
 * latches" in HW_AUDIO_SCAFFOLD_PLAN.md.  Other consumers should use
 * the const accessor above. */
M4ARegisterFile *m4a_get_register_file_mut(M4ADriver *drv);

/* Layer 1.5 event-stream accessors.  Every m4a_advance() call appends
 * register-write events to the per-render-span queue, tagged with the
 * sample_offset (host-frame relative to the start of the *current*
 * render span — i.e. since the last m4a_consume_writes call).  Chip
 * calls m4a_get_pending_writes() to read the batch, then
 * m4a_consume_writes() after applying them.  The latter clears the
 * queue and resets the sample_offset counter.
 *
 * Capacity / chunking: the queue is bounded.  Each vblank emits up to
 * ~40 events (4 channels × 5 NRxx writes + NR51 + 16 wave-RAM bytes
 * on a fresh wave-channel note).  Render call sites should chunk
 * m4a_advance into windows of at most M4A_RECOMMENDED_MAX_ADVANCE_FRAMES
 * and call hw_audio_render_events + m4a_consume_writes between chunks
 * — otherwise events overflow.  m4a_get_events_dropped() returns a
 * monotonic counter incremented on overflow; tests assert it stays 0. */
#define M4A_RECOMMENDED_MAX_ADVANCE_FRAMES 2048

const M4ARegWriteBatch *m4a_get_pending_writes(const M4ADriver *drv);
void                    m4a_consume_writes(M4ADriver *drv);
uint32_t                m4a_get_events_dropped(const M4ADriver *drv);

#ifdef __cplusplus
}
#endif

#endif
