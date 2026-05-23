#ifndef M4A_INTERNAL_H
#define M4A_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "m4a_driver.h"
#include "m4a_register_file.h"
#include "m4a_pcm_ring.h"
#include "voicegroup/voicegroup_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M4A_MAX_TRACKS         16
#define M4A_MAX_CGB_CHANNELS   4   /* sq1, sq2, wave, noise */
#define M4A_MAX_PCM_CHANNELS  12   /* DirectSound polyphony cap */

/* Layer 1.5 event-queue capacity.  Per render span (between consume
 * calls): at most ~5 NRxx writes per CGB channel × 4 channels = 20
 * per vblank for snapshot-equivalent emits, plus 16 wave-RAM bytes
 * on full rewrites, plus occasional NR50/51/SOUNDCNT_H writes.  256
 * comfortably holds several vblanks of dense activity. */
#define M4A_EVENT_QUEUE_CAP    256

/* PCM_DMA constants live in m4a_pcm_ring.h (public — chip-side reads
 * them too).  M4A_PCM_SAMPLES_PER_VBLANK / M4A_PCM_RATE_HZ /
 * M4A_PCM_DMA_BUF_SIZE come in via that header, included above. */

/* m4a runs at 59.7275 Hz vblank.  Use 1/this as the period for tick firing. */
#define M4A_VBLANK_HZ          59.7275f

/* Channel `status` flag bits — match pokeemerald m4a_internal.h CHN_*.
 * We mirror the real m4a constants so disasm reads match. */
#define M4A_CHN_ON              0x80
#define M4A_CHN_STOP            0x40
#define M4A_CHN_LOOP            0x10
#define M4A_CHN_START           0x20
#define M4A_CHN_IEC             0x04   /* In Echo/pseudo-echo */
/* Envelope phase ordinals.  Encoded so `status--` walks the envelope
 * forward (ATTACK→DECAY→SUSTAIN→RELEASE), matching v1 m4a_engine.h and
 * pokeemerald m4a_internal.h.  m4a_cgb.c / m4a_pcm.c rely on this
 * ordering when transitioning between phases. */
#define M4A_CHN_ENV_MASK        0x03
#define M4A_CHN_ENV_RELEASE     0x00
#define M4A_CHN_ENV_SUSTAIN     0x01
#define M4A_CHN_ENV_DECAY       0x02
#define M4A_CHN_ENV_ATTACK      0x03

/* CgbSound `modify` bits — pokeemerald m4a_internal.h MO_*. */
#define M4A_MO_PIT              0x1   /* re-emit NRx3 + NRx4 freq write */
#define M4A_MO_VOL              0x2   /* re-emit NRx2 + NRx4-with-trigger */

/* Driver-internal track state.  Shape mirrors v1's M4ATrack so the disasm
 * comparison path stays clean.  Field set is the minimum needed for Layer 1
 * (CGB envelope/pitch); PCM-specific fields land in step 2. */
typedef struct {
    uint8_t  flags;
    uint8_t  volume;        /* CC7 vol scaled by song master volume */
    uint8_t  rawVolume;     /* CC7 raw before song-master scaling */
    uint8_t  volX;          /* xCmd external volume multiplier (0..64) */
    int8_t   pan;           /* CC10 -64..+63 */
    int8_t   panX;
    int8_t   bend;          /* pitch bend signed -64..+63 */
    uint8_t  bendRange;     /* 1..12 semitones (default 2) */

    uint8_t  lfoSpeed;
    uint8_t  lfoSpeedC;
    uint8_t  lfoDelay;
    uint8_t  lfoDelayC;
    uint8_t  mod;           /* mod depth */
    uint8_t  modT;          /* 0=vibrato 1=tremolo 2=autopan */
    int8_t   modM;          /* current mod output */

    int8_t   keyShift;
    int8_t   keyShiftX;
    int8_t   tune;
    uint8_t  pitX;

    /* Computed by m4a_trk_vol_pit_set */
    int8_t   keyM;
    uint8_t  pitM;
    uint8_t  volMR;
    uint8_t  volML;

    uint8_t  pseudoEchoVolume;
    uint8_t  pseudoEchoLength;
    uint8_t  priority;

    /* xCmd (XCMD-via-MIDI-CC) state.  Two-CC protocol mirrors v1
     * (plugin/m4a_engine.c:779-796): CC 0x1E sets `extendedCommand`,
     * CC 0x1D/0x1F appends payload bytes.  When `extendedCommandCount`
     * reaches `xcmd_data_length(extendedCommand)` we apply.  Selector is
     * sticky after apply — only the byte count resets.  See xcmd.md. */
    uint8_t   extendedCommand;        /* last 0x1E selector, 0 = idle */
    uint8_t   extendedCommandCount;   /* bytes accumulated so far */
    uint8_t   extendedCommandBytes[4];
    uint32_t  extendedValue;          /* xCmd 0x0D payload (notify + storage) */

    uint8_t   currentProgram;   /* last program_change */
    ToneData  currentVoice;     /* resolved voice for currentProgram */
} M4ADriverTrack;

/* Driver-internal CGB channel state (one per square 1, square 2, wave,
 * noise — total 4).  Equivalent to v1's M4ACGBChannel, narrowed to driver
 * concerns: hardware *runtime* state (phase accum, LFSR, declick) lives in
 * hw_audio.  This struct is what m4a software ticks. */
typedef struct {
    uint8_t  status;          /* M4A_CHN_* flags */
    uint8_t  type;            /* 1=sq1, 2=sq2, 3=wave, 4=noise */
    uint8_t  rightVolume;
    uint8_t  leftVolume;
    uint8_t  attack;
    uint8_t  decay;
    uint8_t  sustain;
    uint8_t  release;
    uint8_t  key;
    uint8_t  envelopeVolume;  /* current 0..15 */
    uint8_t  envelopeGoal;    /* target on note-on */
    uint8_t  sustainGoal;     /* sustain level scaled by envelopeGoal */
    uint8_t  envelopeCounter;
    uint8_t  pseudoEchoVolume;
    uint8_t  pseudoEchoLength;
    uint8_t  midiKey;
    uint8_t  velocity;
    uint8_t  priority;
    int8_t   rhythmPan;
    uint8_t  gateTime;
    uint8_t  length;          /* NRx1 length-counter init value */
    uint8_t  sweep;           /* NR10 sweep byte (sq1 only) */
    uint8_t  dutyCycle;       /* sq1/sq2 duty 0..3 */
    uint8_t  pan;             /* NR51-style L/R nibble */
    uint8_t  panMask;
    uint8_t  modify;          /* M4A_MO_* bits — what needs writing this tick */

    uint16_t frequency;       /* sq1/sq2/wave: 11-bit GB freq.
                                noise: NR43 byte (clock_shift<<4|divisor_code) | period_bit */
    uint32_t *wavePointer;    /* programmable wave (32 nibbles, 16 bytes) */

    int trackIndex;

    /* Set by m4a_drv_cgb_start for wave channels (type 3) to mark the
     * wave RAM as dirty.  Next emit_vol_write fires 16 byte-granular
     * WAVE_RAM_BYTE events and clears the flag.  Mirrors real m4a's
     * STMIA-write pattern in CgbSound when the chip writes a fresh
     * wave to NR_3 (only safe while wave_dac_on=false). */
    bool      waveRamPending;

    /* Set by m4a_drv_cgb_start; consumed-and-cleared by the next
     * emit_vol_write to drive the NRx4 trigger bit.  Real m4a only
     * sets the trigger bit on note start; envelope-update MO_VOL
     * ticks must NOT re-trigger because real-GB hardware resets the
     * wave RAM position (NR34) and noise LFSR (NR44) on trigger. */
    bool      freshStart;
} M4ADriverCgbChan;

/* Driver-internal PCM channel state.  Mirrors v1's M4APCMChannel.  All
 * voice synthesis happens here in software (real m4a's SoundMainRAM);
 * the hardware FIFOs only ever see the post-mix int8 stream. */
typedef struct {
    uint8_t   status;                /* M4A_CHN_* flags */
    uint8_t   type;                  /* voice type (incl. VOICE_TYPE_FIX bit) */
    uint8_t   rightVolume;
    uint8_t   leftVolume;
    uint8_t   attack;
    uint8_t   decay;
    uint8_t   sustain;
    uint8_t   release;
    uint8_t   key;
    uint8_t   envelopeVolume;        /* current 0..255 */
    uint8_t   envelopeVolumeRight;   /* env × right × master, computed per tick */
    uint8_t   envelopeVolumeLeft;
    uint8_t   pseudoEchoVolume;
    uint8_t   pseudoEchoLength;
    uint8_t   midiKey;
    uint8_t   velocity;
    uint8_t   priority;
    int8_t    rhythmPan;
    uint8_t   gateTime;

    /* Sample-read state */
    WaveData *wav;
    int8_t   *currentPointer;
    int32_t   count;                 /* remaining samples in current segment */
    uint32_t  fw;                    /* 23-bit fractional position accumulator */
    uint32_t  frequency;             /* per-PCM-tick step, Q9.23 */

    bool      isLoop;
    int32_t   loopLen;
    int8_t   *loopStart;

    int       trackIndex;
} M4ADriverPcmChan;

struct M4ADriver {
    /* Lifecycle / config */
    float           host_rate;
    ToneData       *voicegroup;
    M4ADriverXcmdFn xcmd_fn;
    void           *xcmd_ctx;

    /* Plugin-level config (mirrored from host/GUI; not yet audible — wired
     * for §12a's "no shadow state" goal). */
    uint8_t  song_volume;
    uint8_t  master_volume;
    uint8_t  reverb_amount;
    bool     analog_filter;
    uint8_t  max_pcm_channels;
    bool     mono_output;
    double   tempo_bpm;

    /* m4a tempo accumulator (vblank-clocked).  Fires LFO ticks when tempoC
     * crosses 150.  Mirrors v1's tempoD/tempoU/tempoI/tempoC. */
    uint16_t tempoD;
    uint16_t tempoU;
    uint16_t tempoI;
    uint16_t tempoC;

    /* CgbSound c15 counter (0..14 cycle for double-step on c15==0). */
    uint8_t  c15;

    /* Per-vblank firing.  m4a_advance accumulates host frames; one vblank
     * fires every host_rate / M4A_VBLANK_HZ frames.  `vblank_accum` is
     * `double` so cumulative-add error stays bounded across long runs
     * with many small m4a_advance calls — a `float` accumulator
     * accumulates per-call rounding that shifts vblank firings between
     * chunkings (visible as PCM chunk-size-invariance drift). */
    double   vblank_step;        /* host_frames per vblank */
    double   vblank_accum;       /* runs forward; subtracts vblank_step on fire */

    /* Layer 1.5 event queue.  CgbSound (and any future MIDI-driven
     * register-write emitter) appends to events[] in chronological
     * order; sample_offset is render-span-relative.  Reset by
     * m4a_consume_writes(). */
    uint32_t     event_render_offset;     /* host frames since last consume */
    uint32_t     event_vblank_offset;     /* offset of current vblank firing */
    size_t       event_count;
    /* Diagnostic: incremented every time m4a_internal_emit_event finds
     * the queue full and has to drop a write.  Production code should
     * keep this at 0 by chunking m4a_advance calls into windows that
     * fit in M4A_EVENT_QUEUE_CAP.  Tests assert it doesn't grow. */
    uint32_t     events_dropped;
    M4ARegWrite  events[M4A_EVENT_QUEUE_CAP];
    /* Stable batch view returned by m4a_get_pending_writes; rebuilt
     * whenever events[] changes.  Keeps the const-correct shape from
     * the public API without exposing the raw array directly. */
    M4ARegWriteBatch event_batch;

    /* Track + channel state */
    M4ADriverTrack    tracks[M4A_MAX_TRACKS];
    M4ADriverCgbChan  cgb[M4A_MAX_CGB_CHANNELS];
    M4ADriverPcmChan  pcmChans[M4A_MAX_PCM_CHANNELS];

    /* SoundMainRAM intermediate mix buffer (int16 stereo, pre-clamp,
     * pre-reverb).  Reverb runs in-place here; results are clamped to
     * int8 and written into the public M4APcmRing.  Sized for one
     * vblank's worth of PCM-rate samples. */
    int16_t  pcmMixL[M4A_PCM_SAMPLES_PER_VBLANK];
    int16_t  pcmMixR[M4A_PCM_SAMPLES_PER_VBLANK];

    /* Reverb delay-line state.  Vanilla Sappy SoundMainRAM_Reverb is a
     * 4-tap design: read current L+R and (current+frameSize) L+R, sum,
     * scale by amount>>9, write back.  Buffer size = canonical PCM DMA
     * buffer (1584 samples per side, matches gPcmDmaBuffer); the "other"
     * tap is one vblank (224 samples) ahead in the circular buffer.
     *
     * Type is int8 because real m4a's reverb buffer IS the int8 FIFO
     * buffer (gPcmDmaBuffer) — wet samples are clamped to int8 range
     * before writeback so future tap reads see the same values that
     * would have been DMA'd.  v1 uses int8 too (m4a_reverb.c).  An
     * int16 buffer here would diverge on heavy mixes where pcmMix
     * temporarily exceeds [-128, 127] before the final clamp-to-int8
     * stage; the delay line would feed those out-of-range values back
     * into subsequent reverb sums, drifting from real-hardware behavior. */
    int8_t   reverbBufL[M4A_PCM_DMA_BUF_SIZE];
    int8_t   reverbBufR[M4A_PCM_DMA_BUF_SIZE];
    uint16_t reverbPos;

    /* Public contract output (driver→chip).  CgbSound writes regs each
     * tick; SoundMainRAM writes pcm.ring_a/ring_b each vblank. */
    M4ARegisterFile   regs;
    M4APcmRing        pcm;
};

/* Internal helpers (exposed to other m4a_*.c files). */
void m4a_internal_recompute_vblank_step(M4ADriver *drv);

/* Append one register-write event at the current vblank offset.  Drops
 * silently if the queue is full (caller's responsibility to size it
 * for the worst case — see M4A_EVENT_QUEUE_CAP). */
void m4a_internal_emit_event(M4ADriver *drv, M4ARegId reg, uint32_t value);

/* Run one LFO tempo tick across all tracks.  Called from m4a_main.c's
 * tempoC-overflow loop.  Mirrors v1's m4a_lfo_tick (m4a_engine.c:870):
 * each track with mod != 0 and lfoSpeed != 0 advances lfoSpeedC by
 * lfoSpeed, derives a triangle-wave sample, and folds it into modM.
 * When modM changes, the track's derived state is recomputed and
 * active CGB / PCM channels on this track are refreshed. */
void m4a_internal_lfo_tick(M4ADriver *drv);

#ifdef __cplusplus
}
#endif

#endif
