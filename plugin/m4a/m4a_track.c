#include "m4a_internal.h"
#include "m4a_freq.h"

#include <string.h>

#define M4A_MAX_SONG_VOLUME 127

/* Forward decls — provided by m4a_cgb.c / m4a_pcm.c.  Kept private to
 * plugin/m4a/. */
extern void m4a_chn_vol_set_cgb(M4ADriverCgbChan *ch, bool mono);
extern void m4a_drv_cgb_start(M4ADriverCgbChan *ch);
extern void m4a_drv_cgb_disable(M4ADriver *drv, M4ADriverCgbChan *ch, int idx);
extern void m4a_drv_pcm_start(M4ADriverPcmChan *ch, WaveData *wav, uint8_t type);

/* TrkVolPitSet — pokeemerald m4a.c equivalent.
 * Recomputes the per-track effective L/R volume (volMR/volML) and effective
 * key+pitch (keyM/pitM) from the current CC + LFO + bend state.  Callable any
 * time a vol/pan/bend/mod/tune CC arrives; CgbSound's per-vblank refresh
 * picks up the new values automatically. */
void m4a_trk_vol_pit_set(M4ADriverTrack *track) {
    int32_t x = ((uint32_t)track->volume * track->volX) >> 5;

    if (track->modT == 1)
        x = ((uint32_t)x * (track->modM + 128)) >> 7;

    int32_t y = 2 * track->pan + track->panX;
    if (track->modT == 2)
        y += track->modM;

    if (y < -128) y = -128;
    else if (y > 127) y = 127;

    track->volMR = (uint32_t)((y + 128) * x) >> 8;
    track->volML = (uint32_t)((127 - y) * x) >> 8;

    int32_t bend = (int32_t)track->bend * track->bendRange;
    int32_t pitchVal = (track->tune + bend) * 4
                     + ((int32_t)track->keyShift << 8)
                     + ((int32_t)track->keyShiftX << 8)
                     + track->pitX;

    if (track->modT == 0)
        pitchVal += 16 * track->modM;

    track->keyM = (int8_t)(pitchVal >> 8);
    track->pitM = (uint8_t)pitchVal;
}

/* Resolve the active voice for `voice` at `key` — handles keysplit /
 * keysplit-all (rhythm) entries.  Mirrors v1 resolve_voice. */
static ToneData *resolve_voice(ToneData *voice, uint8_t key) {
    if (!voice) return NULL;

    uint8_t type = voice->type;

    if (type & VOICE_KEYSPLIT_ALL) {
        ToneData *subGroup = (ToneData *)voice->subGroup;
        if (!subGroup) return NULL;
        ToneData *resolved = &subGroup[key];
        if (resolved->type & (VOICE_KEYSPLIT | VOICE_KEYSPLIT_ALL))
            return NULL;
        return resolved;
    }

    if (type & VOICE_KEYSPLIT) {
        ToneData *subGroup = (ToneData *)voice->subGroup;
        uint8_t *splitTable = voice->keySplitTable;
        if (!subGroup || !splitTable) return NULL;
        uint8_t idx = splitTable[key];
        ToneData *resolved = &subGroup[idx];
        if (resolved->type & (VOICE_KEYSPLIT | VOICE_KEYSPLIT_ALL))
            return NULL;
        return resolved;
    }

    return voice;
}

/* Re-push pitch into every active PCM channel on this track.  Mirrors
 * v1 refresh_channel_pitches' PCM half: recompute fw step from the
 * track's keyM/pitM, applying the same divFreq scaling note_on uses. */
static void refresh_pcm_pitches(M4ADriver *drv, int trackIndex) {
    M4ADriverTrack *track = &drv->tracks[trackIndex];
    const int pcmFreq = M4A_PCM_RATE_HZ;
    const int divFreq = (16777216 / pcmFreq + 1) >> 1;
    for (int i = 0; i < M4A_MAX_PCM_CHANNELS; i++) {
        M4ADriverPcmChan *ch = &drv->pcmChans[i];
        if (!(ch->status & M4A_CHN_ON)) continue;
        if (ch->trackIndex != trackIndex)  continue;
        if (!ch->wav)                      continue;
        if (ch->type & VOICE_TYPE_FIX)     continue;  /* fixed pitch */

        int32_t finalKey = (int32_t)ch->key + track->keyM;
        if (finalKey < 0)   finalKey = 0;
        if (finalKey > 127) finalKey = 127;
        uint32_t f = m4a_midi_key_to_freq(ch->wav, (uint8_t)finalKey, track->pitM);
        ch->frequency = (uint32_t)((uint64_t)f * (uint64_t)divFreq);
    }
}

/* Re-push volume/pan into every active PCM channel on this track.  PCM
 * voices don't go through CGB pan-mask / NR51 — they get straight L/R
 * volumes that the SoundMainRAM mixer reads per output sample. */
static void refresh_pcm_volumes(M4ADriver *drv, int trackIndex) {
    M4ADriverTrack *track = &drv->tracks[trackIndex];
    for (int i = 0; i < M4A_MAX_PCM_CHANNELS; i++) {
        M4ADriverPcmChan *ch = &drv->pcmChans[i];
        if (!(ch->status & M4A_CHN_ON)) continue;
        if (ch->trackIndex != trackIndex)  continue;

        uint32_t velo = ch->velocity;
        int32_t  rp   = ch->rhythmPan;

        uint32_t panR = (uint32_t)(0x80 + rp);
        uint32_t volR = panR * velo;
        uint32_t res  = (volR * track->volMR) >> 14;
        if (res > 0xFF) res = 0xFF;
        ch->rightVolume = (uint8_t)res;

        uint32_t panL = (uint32_t)(0x7F - rp);
        uint32_t volL = panL * velo;
        res = (volL * track->volML) >> 14;
        if (res > 0xFF) res = 0xFF;
        ch->leftVolume = (uint8_t)res;
        /* envelopeVolumeLeft/Right is recomputed from leftVolume *
         * envelopeVolume * masterVolume on the next pcm_channel_tick. */
    }
}

/* Re-push pitch into every active CGB channel on this track.  Called when a
 * track's effective key changes (pitch bend, bend range, tune, LFO vibrato). */
static void refresh_cgb_pitches(M4ADriver *drv, int trackIndex) {
    M4ADriverTrack *track = &drv->tracks[trackIndex];
    for (int i = 0; i < M4A_MAX_CGB_CHANNELS; i++) {
        M4ADriverCgbChan *ch = &drv->cgb[i];
        if (!(ch->status & M4A_CHN_ON)) continue;
        if (ch->trackIndex != trackIndex) continue;

        int32_t finalKey = (int32_t)ch->key + track->keyM;
        if (finalKey < 0) finalKey = 0;
        if (finalKey > 127) finalKey = 127;
        uint32_t newFreq = m4a_midi_key_to_cgb_freq(ch->type, (uint8_t)finalKey, track->pitM);
        if (ch->type == 4)
            newFreq |= ch->frequency & 0x08;  /* preserve NR43 width bit */
        if ((uint16_t)newFreq != ch->frequency) {
            ch->frequency = (uint16_t)newFreq;
            ch->modify |= M4A_MO_PIT;
        }
    }
}

/* Re-push volume/pan into every active CGB channel on this track. */
static void refresh_cgb_volumes(M4ADriver *drv, int trackIndex) {
    M4ADriverTrack *track = &drv->tracks[trackIndex];
    for (int i = 0; i < M4A_MAX_CGB_CHANNELS; i++) {
        M4ADriverCgbChan *ch = &drv->cgb[i];
        if (!(ch->status & M4A_CHN_ON)) continue;
        if (ch->trackIndex != trackIndex) continue;

        uint8_t prevR = ch->rightVolume;
        uint8_t prevL = ch->leftVolume;
        uint32_t velocity = ch->velocity;
        int32_t rhythmPan = ch->rhythmPan;

        uint32_t panR = (uint32_t)(0x80 + rhythmPan);
        uint32_t volR = panR * velocity;
        uint32_t result = (volR * track->volMR) >> 14;
        if (result > 0xFF) result = 0xFF;
        ch->rightVolume = (uint8_t)result;

        uint32_t panL = (uint32_t)(0x7F - rhythmPan);
        uint32_t volL = panL * velocity;
        result = (volL * track->volML) >> 14;
        if (result > 0xFF) result = 0xFF;
        ch->leftVolume = (uint8_t)result;

        if (ch->rightVolume != prevR || ch->leftVolume != prevL) {
            m4a_chn_vol_set_cgb(ch, drv->mono_output);
            ch->modify |= M4A_MO_VOL;
        }
    }
}

/* ---- Public ingress ---- */

void m4a_program_change(M4ADriver *drv, int track, uint8_t program) {
    if (!drv) return;
    if (track < 0 || track >= M4A_MAX_TRACKS) return;
    if (program >= 128) return;

    M4ADriverTrack *t = &drv->tracks[track];
    t->currentProgram = program;
    if (drv->voicegroup)
        t->currentVoice = drv->voicegroup[program];
    else
        memset(&t->currentVoice, 0, sizeof(t->currentVoice));
}

void m4a_note_on(M4ADriver *drv, int track, uint8_t key, uint8_t velocity) {
    if (!drv) return;
    if (track < 0 || track >= M4A_MAX_TRACKS) return;

    M4ADriverTrack *t = &drv->tracks[track];
    ToneData *voice = resolve_voice(&t->currentVoice, key);
    if (!voice) return;

    uint8_t voiceType = voice->type & VOICE_TYPE_CGB_MASK;
    int8_t  rhythmPan = 0;
    uint8_t useKey = key;

    if (t->currentVoice.type & VOICE_KEYSPLIT_ALL) {
        useKey = voice->key;
        if (voice->panSweep & 0x80)
            rhythmPan = (int8_t)((voice->panSweep - 0xC0) * 2);
    }

    /* Refresh derived track values before computing channel state. */
    m4a_trk_vol_pit_set(t);

    int32_t finalKey = (int32_t)useKey + t->keyM;
    if (finalKey < 0)   finalKey = 0;
    if (finalKey > 127) finalKey = 127;

    uint8_t combinedPriority = t->priority;

    if (voiceType >= 1 && voiceType <= 4) {
        int cgbIdx = voiceType - 1;
        M4ADriverCgbChan *ch = &drv->cgb[cgbIdx];

        /* Priority steal: lower numbers win.  Within equal priority, lower
         * track index wins (matches v1 / m4a behaviour). */
        if ((ch->status & M4A_CHN_ON) && !(ch->status & M4A_CHN_STOP)) {
            if (ch->priority > combinedPriority)                                 return;
            if (ch->priority == combinedPriority && ch->trackIndex < track)      return;
        }

        ch->midiKey   = key;
        ch->key       = useKey;
        ch->velocity  = velocity;
        ch->priority  = combinedPriority;
        ch->trackIndex = track;
        ch->rhythmPan = rhythmPan;
        ch->attack    = voice->attack;
        ch->decay     = voice->decay;
        ch->sustain   = voice->sustain;
        ch->release   = voice->release;
        ch->pseudoEchoVolume = t->pseudoEchoVolume;
        ch->pseudoEchoLength = t->pseudoEchoLength;
        ch->length    = voice->length;
        ch->gateTime  = 0;

        /* L/R software volumes from velocity × track vol × pan. */
        uint32_t velo = ch->velocity;
        int32_t  rp   = ch->rhythmPan;
        uint32_t panR = (uint32_t)(0x80 + rp);
        uint32_t volR = panR * velo;
        uint32_t res  = (volR * t->volMR) >> 14;
        if (res > 0xFF) res = 0xFF;
        ch->rightVolume = (uint8_t)res;

        uint32_t panL = (uint32_t)(0x7F - rp);
        uint32_t volL = panL * velo;
        res = (volL * t->volML) >> 14;
        if (res > 0xFF) res = 0xFF;
        ch->leftVolume = (uint8_t)res;

        /* Compute envelope goal + NR51 pan mask. */
        m4a_chn_vol_set_cgb(ch, drv->mono_output);

        /* Voice-specific bits: duty (sq1/sq2), sweep (sq1), wave RAM (wave). */
        if (voiceType == 1 || voiceType == 2) {
            ch->dutyCycle = (uint8_t)(uintptr_t)voice->wavePointer & 0x03;
            if (voiceType == 1)
                ch->sweep = (voice->panSweep & 0x70) ? voice->panSweep : 0x08;
        } else if (voiceType == 3) {
            ch->wavePointer = voice->wavePointer;
        }

        /* Frequency word (sq1/sq2/wave: 11-bit GB freq; noise: NR43 byte). */
        ch->frequency = (uint16_t)m4a_midi_key_to_cgb_freq(voiceType, (uint8_t)finalKey, t->pitM);
        if (voiceType == 4)
            ch->frequency |= ((uintptr_t)voice->wavePointer & 0x01) << 3;

        m4a_drv_cgb_start(ch);
    } else {
        /* DirectSound voice — software mix.  Allocate a PCM slot, set
         * envelope/freq state.  SoundMainRAM (m4a_pcm.c) does the per-
         * vblank mix into M4APcmRing. */
        if (!voice->wav) return;

        M4ADriverPcmChan *ch = NULL;
        M4ADriverPcmChan *bestSteal = NULL;
        uint8_t bestPriority = combinedPriority;
        int     bestTrackIndex = track;
        bool    bestIsStopping = false;
        uint8_t maxPcm = drv->max_pcm_channels;
        if (maxPcm == 0 || maxPcm > M4A_MAX_PCM_CHANNELS) maxPcm = M4A_MAX_PCM_CHANNELS;

        for (int i = 0; i < maxPcm; i++) {
            M4ADriverPcmChan *cand = &drv->pcmChans[i];
            if (!(cand->status & M4A_CHN_ON)) { ch = cand; break; }

            if (cand->status & M4A_CHN_STOP) {
                if (!bestIsStopping) {
                    bestIsStopping = true;
                    bestPriority = cand->priority;
                    bestTrackIndex = cand->trackIndex;
                    bestSteal = cand;
                } else if (cand->priority < bestPriority) {
                    bestPriority = cand->priority;
                    bestTrackIndex = cand->trackIndex;
                    bestSteal = cand;
                } else if (cand->priority == bestPriority
                           && cand->trackIndex >= bestTrackIndex) {
                    bestTrackIndex = cand->trackIndex;
                    bestSteal = cand;
                }
                continue;
            }

            if (!bestIsStopping) {
                if (cand->priority < bestPriority) {
                    bestPriority = cand->priority;
                    bestTrackIndex = cand->trackIndex;
                    bestSteal = cand;
                } else if (cand->priority == bestPriority
                           && cand->trackIndex >= bestTrackIndex) {
                    bestTrackIndex = cand->trackIndex;
                    bestSteal = cand;
                }
            }
        }
        if (!ch) {
            if (!bestSteal) return;
            if (!bestIsStopping && combinedPriority < bestPriority) return;
            ch = bestSteal;
        }

        ch->midiKey       = key;
        ch->key           = useKey;
        ch->velocity      = velocity;
        ch->priority      = combinedPriority;
        ch->trackIndex    = track;
        ch->rhythmPan     = rhythmPan;
        ch->attack        = voice->attack;
        ch->decay         = voice->decay;
        ch->sustain       = voice->sustain;
        ch->release       = voice->release;
        ch->pseudoEchoVolume = t->pseudoEchoVolume;
        ch->pseudoEchoLength = t->pseudoEchoLength;
        ch->gateTime      = 0;

        /* ChnVolSetDirect: per-channel L/R from velocity × pan × track vol. */
        uint32_t velo = ch->velocity;
        int32_t  rp   = ch->rhythmPan;
        uint32_t panR = (uint32_t)(0x80 + rp);
        uint32_t volR = panR * velo;
        uint32_t res  = (volR * t->volMR) >> 14;
        if (res > 0xFF) res = 0xFF;
        ch->rightVolume = (uint8_t)res;

        uint32_t panL = (uint32_t)(0x7F - rp);
        uint32_t volL = panL * velo;
        res = (volL * t->volML) >> 14;
        if (res > 0xFF) res = 0xFF;
        ch->leftVolume = (uint8_t)res;

        /* Per-PCM-tick frequency word.  MidiKeyToFreq's output is in a
         * step-per-second-relative form; the divFreq multiplier converts
         * it to step-per-tick × 2^23 at PCM_RATE_HZ.  Same formula as v1
         * (plugin/m4a_engine.c:590) and pokeemerald m4a_1.s. */
        if (voice->type & VOICE_TYPE_FIX) {
            ch->frequency = 0x800000u;   /* 1 sample per tick, no resample */
        } else {
            const int pcmFreq = M4A_PCM_RATE_HZ;
            const int divFreq = (16777216 / pcmFreq + 1) >> 1;   /* ≈ 627 */
            uint32_t f = m4a_midi_key_to_freq(voice->wav, (uint8_t)finalKey, t->pitM);
            ch->frequency = (uint32_t)((uint64_t)f * (uint64_t)divFreq);
        }

        m4a_drv_pcm_start(ch, voice->wav, voice->type);

        /* Pre-compute envelope volumes so the very first vblank's mix is
         * audible — SoundMainRAM_Tick fires at vblank rate, but the
         * mixer reads envelopeVolumeLeft/Right per-sample. */
        uint32_t mv = (uint32_t)(drv->master_volume + 1) * ch->envelopeVolume;
        mv >>= 4;
        ch->envelopeVolumeRight = (uint8_t)(((uint32_t)ch->rightVolume * mv) >> 8);
        ch->envelopeVolumeLeft  = (uint8_t)(((uint32_t)ch->leftVolume  * mv) >> 8);
    }
}

void m4a_note_off(M4ADriver *drv, int track, uint8_t key) {
    if (!drv) return;
    if (track < 0 || track >= M4A_MAX_TRACKS) return;

    for (int i = 0; i < M4A_MAX_CGB_CHANNELS; i++) {
        M4ADriverCgbChan *ch = &drv->cgb[i];
        if ((ch->status & M4A_CHN_ON) && !(ch->status & M4A_CHN_STOP)
            && ch->trackIndex == track && ch->midiKey == key) {
            ch->status |= M4A_CHN_STOP;
        }
    }
    for (int i = 0; i < M4A_MAX_PCM_CHANNELS; i++) {
        M4ADriverPcmChan *ch = &drv->pcmChans[i];
        if ((ch->status & M4A_CHN_ON) && !(ch->status & M4A_CHN_STOP)
            && ch->trackIndex == track && ch->midiKey == key) {
            ch->status |= M4A_CHN_STOP;
        }
    }
}

/* ---- xCmd (XCMD-via-MIDI-CC) helpers ----------------------------------
 *
 * Direct ports of v1's plugin/m4a_engine.c statics, retargeted to v2's
 * M4ADriverTrack field names.  Selector → byte-count map and LE
 * assembly are byte-for-byte identical; xcmd_apply mutates the same
 * track-level fields (currentVoice.* and pseudoEcho*) the v1 engine
 * touches, so notes started after the xCmd see the changed ADSR /
 * pseudo-echo values via the existing note_on copy in m4a_note_on().
 *
 * Two deviations from v1 documented in xcmd.md and approved upstream:
 *   - 0x01 (xWAVE): notify-only.  v1 stores the 4-byte LE payload as a
 *     pointer cast into currentVoice.wav.  In poryaaaa that integer is
 *     a ROM address from the original game, not a valid host pointer
 *     — applying it would dangle.  An address→loaded-sample resolver
 *     can be added later; until then the callback gets the raw u32 and
 *     plugin code can dispatch.
 *   - 0x0D: store in extendedValue and notify (matches v1).
 */
/* xCmd 0x0C (xWAIT) is intentionally absent — there's no song-script
 * interpreter in the v2 MIDI ingress path that could honour it, so it
 * stays as an unknown selector (dataLength == 0) and any payload bytes
 * are dropped silently. */
static uint8_t xcmd_data_length(uint8_t selector) {
    switch (selector) {
    case 0x01: case 0x0D: return 4;
    case 0x02: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        return 1;
    default:              return 0;
    }
}

static uint32_t xcmd_read_le(const uint8_t *bytes, uint8_t count) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < count; i++)
        value |= (uint32_t)bytes[i] << (i * 8);
    return value;
}

static void xcmd_notify(M4ADriver *drv, int trackIndex,
                        uint8_t selector, uint32_t value) {
    if (drv->xcmd_fn)
        drv->xcmd_fn(drv->xcmd_ctx, trackIndex, selector, value);
}

static void xcmd_apply(M4ADriver *drv, int trackIndex, M4ADriverTrack *t) {
    switch (t->extendedCommand) {
    case 0x01: /* xwave — notify-only; raw u32 is a ROM address, not a host ptr */
        xcmd_notify(drv, trackIndex, 0x01, xcmd_read_le(t->extendedCommandBytes, 4));
        break;
    case 0x02: /* xtype */
        t->currentVoice.type = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x02, t->extendedCommandBytes[0]);
        break;
    case 0x04: /* xatta */
        t->currentVoice.attack = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x04, t->extendedCommandBytes[0]);
        break;
    case 0x05: /* xdeca */
        t->currentVoice.decay = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x05, t->extendedCommandBytes[0]);
        break;
    case 0x06: /* xsust */
        t->currentVoice.sustain = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x06, t->extendedCommandBytes[0]);
        break;
    case 0x07: /* xrele */
        t->currentVoice.release = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x07, t->extendedCommandBytes[0]);
        break;
    case 0x08: /* xiecv */
        t->pseudoEchoVolume = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x08, t->extendedCommandBytes[0]);
        break;
    case 0x09: /* xiecl */
        t->pseudoEchoLength = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x09, t->extendedCommandBytes[0]);
        break;
    case 0x0A: /* xleng */
        t->currentVoice.length = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x0A, t->extendedCommandBytes[0]);
        break;
    case 0x0B: /* xswee */
        t->currentVoice.panSweep = t->extendedCommandBytes[0];
        xcmd_notify(drv, trackIndex, 0x0B, t->extendedCommandBytes[0]);
        break;
    case 0x0D:
        t->extendedValue = xcmd_read_le(t->extendedCommandBytes, 4);
        xcmd_notify(drv, trackIndex, 0x0D, t->extendedValue);
        break;
    default:
        break;
    }

    /* Selector stays sticky (matches v1).  Only the byte-count resets so
     * a subsequent 0x1D/0x1F starts refilling for the same selector. */
    t->extendedCommandCount = 0;
}

void m4a_cc(M4ADriver *drv, int track, uint8_t cc, uint8_t value) {
    if (!drv) return;
    if (track < 0 || track >= M4A_MAX_TRACKS) return;
    M4ADriverTrack *t = &drv->tracks[track];

    switch (cc) {
    case 0x01:  /* Mod wheel → LFO depth */
        t->mod = value;
        if (value == 0) {
            t->lfoSpeedC = 0;
            t->modM = 0;
        }
        break;
    case 0x07:  /* Volume */
        t->rawVolume = value;
        t->volume = value * drv->song_volume / M4A_MAX_SONG_VOLUME;
        m4a_trk_vol_pit_set(t);
        refresh_cgb_volumes(drv, track);
        refresh_pcm_volumes(drv, track);
        break;
    case 0x0A:  /* Pan */
        t->pan = (int8_t)(value - 64);
        m4a_trk_vol_pit_set(t);
        refresh_cgb_volumes(drv, track);
        refresh_pcm_volumes(drv, track);
        break;
    case 0x14:  /* BENDR */
        t->bendRange = value;
        m4a_trk_vol_pit_set(t);
        refresh_cgb_pitches(drv, track);
        refresh_pcm_pitches(drv, track);
        break;
    case 0x15:  /* LFOS */
        t->lfoSpeed = value;
        t->lfoSpeedC = 0;
        t->modM = 0;
        break;
    case 0x16:  /* MODT */
        if (t->modT != value) {
            t->modT = value;
            /* Re-derive volMR/volML/keyM/pitM before pushing into
             * channels — modT switches *which* axis modM modulates
             * (vibrato/tremolo/autopan), so the derived track values
             * change even though raw vol/pan/bend didn't.  V1
             * refresh_volumes does this implicitly via the embedded
             * trk_vol_pit_set call. */
            m4a_trk_vol_pit_set(t);
            refresh_cgb_volumes(drv, track);
            refresh_cgb_pitches(drv, track);
            refresh_pcm_volumes(drv, track);
            refresh_pcm_pitches(drv, track);
        }
        break;
    case 0x18:  /* TUNE */
        if (t->tune != (int8_t)(value - 0x40)) {
            t->tune = (int8_t)(value - 0x40);
            m4a_trk_vol_pit_set(t);
            refresh_cgb_pitches(drv, track);
            refresh_pcm_pitches(drv, track);
        }
        break;
    case 0x1A:  /* LFODL */
        if (t->lfoDelay != value || t->lfoDelayC != value
            || t->lfoSpeedC != 0 || t->modM != 0) {
            t->lfoDelay = value;
            t->lfoDelayC = value;
            t->lfoSpeedC = 0;
            if (t->modM != 0) {
                t->modM = 0;
                m4a_trk_vol_pit_set(t);
                refresh_cgb_volumes(drv, track);
                refresh_cgb_pitches(drv, track);
                refresh_pcm_volumes(drv, track);
                refresh_pcm_pitches(drv, track);
            }
        }
        break;
    case 0x1D:  /* XCMD payload byte (part 1) */
    case 0x1F: { /* XCMD payload byte (alternate) */
        uint8_t dataLength = xcmd_data_length(t->extendedCommand);
        if (dataLength == 0) break;   /* no selector latched, or unknown selector */
        if (t->extendedCommandCount >= 4) break;  /* defensive — shouldn't happen */
        t->extendedCommandBytes[t->extendedCommandCount++] = value;
        if (t->extendedCommandCount >= dataLength)
            xcmd_apply(drv, track, t);
        break;
    }
    case 0x1E:  /* XCMD selector (part 2 — sets which xCmd to assemble) */
        t->extendedCommand = value;
        t->extendedCommandCount = 0;
        memset(t->extendedCommandBytes, 0, sizeof(t->extendedCommandBytes));
        break;
    case 0x7B:  /* All Notes Off */
        m4a_all_notes_off(drv, track);
        break;
    case 0x78:  /* All Sound Off */
        m4a_all_sound_off(drv);
        break;
    default:
        break;
    }
}

void m4a_pitch_bend(M4ADriver *drv, int track, int16_t bend) {
    if (!drv) return;
    if (track < 0 || track >= M4A_MAX_TRACKS) return;
    M4ADriverTrack *t = &drv->tracks[track];
    t->bend = (int8_t)(bend >> 7);
    m4a_trk_vol_pit_set(t);
    refresh_cgb_pitches(drv, track);
    refresh_pcm_pitches(drv, track);
}

void m4a_all_notes_off(M4ADriver *drv, int track) {
    if (!drv) return;
    if (track < 0 || track >= M4A_MAX_TRACKS) return;
    for (int i = 0; i < M4A_MAX_CGB_CHANNELS; i++) {
        M4ADriverCgbChan *ch = &drv->cgb[i];
        if ((ch->status & M4A_CHN_ON) && ch->trackIndex == track)
            ch->status |= M4A_CHN_STOP;
    }
    for (int i = 0; i < M4A_MAX_PCM_CHANNELS; i++) {
        M4ADriverPcmChan *ch = &drv->pcmChans[i];
        if ((ch->status & M4A_CHN_ON) && ch->trackIndex == track)
            ch->status |= M4A_CHN_STOP;
    }
}

/* m4a_internal_lfo_tick — pokeemerald m4a.c equivalent (mirrors v1's
 * m4a_lfo_tick at m4a_engine.c:870).  Fired by m4a_main.c's tempoC
 * overflow loop (tempoI/150 ticks per vblank).  For each track with
 * an active modulator (mod != 0 and lfoSpeed != 0):
 *
 *   1. If lfoDelayC > 0, decrement and skip — the modulator is in its
 *      "delay" phase post-trigger.
 *   2. Advance lfoSpeedC by lfoSpeed (wraps at 256 — wraparound is
 *      what gives the triangle wave its period).
 *   3. Convert lfoSpeedC into a signed triangle-wave sample lfoVal in
 *      [-0x40 .. +0x3F] using the canonical "abs of signed minus 0x40"
 *      pattern.
 *   4. Fold mod depth: newModM = (mod * lfoVal) >> 6.
 *   5. If newModM differs from current modM, store it, recompute
 *      track-derived state via m4a_trk_vol_pit_set, and refresh active
 *      CGB / PCM channels on this track.  Volume axis (volMR/volML) is
 *      always refreshed; pitch axis is refreshed only when modT == 0
 *      (vibrato — v1 m4a_engine.c:920 condition).  modT == 1 (tremolo)
 *      and modT == 2 (autopan) only need volume. */
void m4a_internal_lfo_tick(M4ADriver *drv) {
    if (!drv) return;
    for (int i = 0; i < M4A_MAX_TRACKS; i++) {
        M4ADriverTrack *t = &drv->tracks[i];
        if (t->lfoSpeed == 0 || t->mod == 0) continue;

        if (t->lfoDelayC > 0) {
            t->lfoDelayC--;
            continue;
        }

        t->lfoSpeedC += t->lfoSpeed;
        uint8_t lfoPos = t->lfoSpeedC;
        int8_t  lfoVal;
        /* Triangle wave: first half of period (lfoPos in 0..0x3F)
         * ramps up from 0 to +0x3F; second half (0x40..0xFF) folds
         * back via lfoVal = 0x80 - lfoPos, ramping from +0x40 down
         * through 0 to -0x80 then back to 0. */
        if ((int8_t)(lfoPos - 0x40) < 0)
            lfoVal = (int8_t)lfoPos;
        else
            lfoVal = (int8_t)(0x80 - lfoPos);

        int8_t newModM = (int8_t)(((int32_t)t->mod * lfoVal) >> 6);
        if (newModM != t->modM) {
            t->modM = newModM;
            m4a_trk_vol_pit_set(t);
            refresh_cgb_volumes(drv, i);
            refresh_pcm_volumes(drv, i);
            if (t->modT == 0) {
                /* Vibrato: pitch axis follows modM. */
                refresh_cgb_pitches(drv, i);
                refresh_pcm_pitches(drv, i);
            }
        }
    }
}

/* m4a_set_song_volume — pokeemerald m4a.c equivalent.  Stores new song
 * master, recomputes each track's effective `volume` (= rawVolume *
 * song_volume / 127), and refreshes active CGB channel L/R volumes so
 * the change is audible immediately, not only on the next CC7. */
void m4a_set_song_volume(M4ADriver *drv, uint8_t volume) {
    if (!drv) return;
    drv->song_volume = volume;
    for (int i = 0; i < M4A_MAX_TRACKS; i++) {
        M4ADriverTrack *t = &drv->tracks[i];
        t->volume = (uint32_t)t->rawVolume * volume / M4A_MAX_SONG_VOLUME;
        m4a_trk_vol_pit_set(t);
        refresh_cgb_volumes(drv, i);
        refresh_pcm_volumes(drv, i);
    }
}

void m4a_all_sound_off(M4ADriver *drv) {
    if (!drv) return;
    /* CC 0x78 / panic stop: all channels silent immediately, no envelope
     * release.  Mirrors v1's m4a_engine_all_sound_off → channel_stop, and
     * the real m4a writes that zero NRx2/NR52. */
    for (int i = 0; i < M4A_MAX_CGB_CHANNELS; i++) {
        M4ADriverCgbChan *ch = &drv->cgb[i];
        ch->status = 0;
        ch->modify = 0;
        m4a_drv_cgb_disable(drv, ch, i);
    }
    for (int i = 0; i < M4A_MAX_PCM_CHANNELS; i++) {
        M4ADriverPcmChan *ch = &drv->pcmChans[i];
        ch->status = 0;
        ch->envelopeVolume = 0;
        ch->envelopeVolumeLeft = 0;
        ch->envelopeVolumeRight = 0;
    }
}
