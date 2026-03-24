/*
 * poryaaaa_render - Standalone M4A MIDI renderer
 *
 * Usage: poryaaaa_render <project_root> <voicegroup> --midi <file.mid> [options]
 *
 * Parses a Standard MIDI File (Type 0 or Type 1), renders it through the
 * M4A engine using a specified voicegroup, and writes a WAV file and/or
 * plays audio through the computer's speakers via miniaudio.
 *
 * Loop support: MIDI text events (Meta 0x01) or marker events (Meta 0x06)
 * containing exactly '[' mark the loop start, and ']' mark the loop end.
 * When both are found the song loops with a configurable count and fadeout.
 */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#ifdef __linux__
#include <dlfcn.h>
#endif
#include "m4a_engine.h"
#include "midi_timeline.h"
#include "m4a_reverb.h"
#include "voicegroup_loader.h"

/* ========================================================================
 * WAV writing helpers (matching test_wav_export.c)
 * ======================================================================== */

static void write_u16_le(FILE *f, uint16_t val)
{
    uint8_t buf[2] = { val & 0xFF, (val >> 8) & 0xFF };
    fwrite(buf, 1, 2, f);
}

static void write_u32_le(FILE *f, uint32_t val)
{
    uint8_t buf[4] = { val & 0xFF, (val >> 8) & 0xFF,
                       (val >> 16) & 0xFF, (val >> 24) & 0xFF };
    fwrite(buf, 1, 4, f);
}

static int write_wav(const char *path, const float *left, const float *right,
                     uint64_t numSamples, int sampleRate)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open %s for writing\n", path);
        return -1;
    }

    uint16_t numChannels   = 2;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate   = (uint32_t)sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t dataSize   = (uint32_t)(numSamples * numChannels * bitsPerSample / 8);

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 36 + dataSize);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    write_u32_le(f, 16);
    write_u16_le(f, 1);              /* PCM */
    write_u16_le(f, numChannels);
    write_u32_le(f, (uint32_t)sampleRate);
    write_u32_le(f, byteRate);
    write_u16_le(f, blockAlign);
    write_u16_le(f, bitsPerSample);

    /* data chunk */
    fwrite("data", 1, 4, f);
    write_u32_le(f, dataSize);

    for (uint64_t i = 0; i < numSamples; i++) {
        int32_t l = (int32_t)(left[i]  * 32767.0f);
        int32_t r = (int32_t)(right[i] * 32767.0f);
        if (l >  32767)  l =  32767;
        if (l < -32768)  l = -32768;
        if (r >  32767)  r =  32767;
        if (r < -32768)  r = -32768;
        write_u16_le(f, (uint16_t)(int16_t)l);
        write_u16_le(f, (uint16_t)(int16_t)r);
    }

    fclose(f);
    return 0;
}

/* ========================================================================
 * Miniaudio playback
 * ======================================================================== */

#ifdef __linux__
/*
 * Install a no-op error handler into libasound to suppress the wall of
 * "cannot find card '0'" messages that ALSA prints on WSL and other
 * systems without hardware audio.  Uses dlopen so we don't need to link
 * against libasound explicitly.
 */
static void alsa_error_noop(const char *file, int line, const char *func,
                              int err, const char *fmt, ...)
{
    (void)file; (void)line; (void)func; (void)err; (void)fmt;
}

static void suppress_alsa_errors(void)
{
    /* If libasound is not present this is a no-op */
    void *lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib) return;

    /* void snd_lib_error_set_handler(snd_lib_error_handler_t handler) */
    typedef void (*ErrFn)(const char*, int, const char*, int, const char*, ...);
    typedef void (*SetFn)(ErrFn);
    SetFn setfn;
    *(void **)(&setfn) = dlsym(lib, "snd_lib_error_set_handler");
    if (setfn)
        setfn(alsa_error_noop);
    /* Leave the handle open so the handler stays installed when miniaudio
     * later opens libasound itself (same shared library instance). */
}
#endif /* __linux__ */

typedef struct {
    const float *bufL;
    const float *bufR;
    uint64_t     total;
    uint64_t     pos;
} PlaybackCtx;

static void playback_callback(ma_device *dev, void *out,
                               const void *in, ma_uint32 frameCount)
{
    (void)in;
    PlaybackCtx *ctx = (PlaybackCtx *)dev->pUserData;
    float *dst = (float *)out;
    for (ma_uint32 i = 0; i < frameCount; i++) {
        if (ctx->pos >= ctx->total) {
            dst[i * 2]     = 0.0f;
            dst[i * 2 + 1] = 0.0f;
        } else {
            dst[i * 2]     = ctx->bufL[ctx->pos];
            dst[i * 2 + 1] = ctx->bufR[ctx->pos];
            ctx->pos++;
        }
    }
}

/* ========================================================================
 * CLI
 * ======================================================================== */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <project_root> <voicegroup> --midi <file.mid> [options]\n"
        "\n"
        "Required:\n"
        "  <project_root>              Path to pokeemerald/pokefirered project root\n"
        "  <voicegroup>                Voicegroup name (e.g. petalburg)\n"
        "  --midi <file.mid>           MIDI input file\n"
        "\n"
        "Output (at least one required):\n"
        "  --output <file.wav>         Write rendered audio to WAV file\n"
        "  --play                      Play audio through computer speakers\n"
        "\n"
        "Audio options:\n"
        "  --song-volume <0-127>       Song master volume (default: 127)\n"
        "  --reverb <0-127>            Reverb amount (default: 0)\n"
        "  --analog-filter             Enable GBA analog low-pass filter (default: off)\n"
        "  --polyphony <1-12>          Max simultaneous PCM channels (default: 5)\n"
        "  --sample-rate <hz>          Sample rate in Hz (default: 44100)\n"
        "  --tail <seconds>            Silence after last event, no loop markers (default: 3.0)\n"
        "\n"
        "Loop options (when MIDI contains '[' / ']' text events):\n"
        "  --loop-count <n>            Number of loop body repetitions (default: 2)\n"
        "  --fadeout <seconds>         Fadeout duration after final loop (default: 5.0)\n"
        "  --total-duration-seconds <s>  Override loop-count; set exact total duration\n"
        "                                (fadeout occupies the final --fadeout seconds)\n",
        prog);
}

/* Dispatch one parsed MIDI event to the engine */
static void dispatch_event(M4AEngine *engine, const MidiRenderEvent *ev,
                            int useTrackIndex)
{
    /* For Type 1 MIDIs, use the SMF track number as the engine track index
     * so that tracks sharing the same MIDI channel get separate engine tracks.
     * For Type 0, fall back to MIDI channel. */
    int trackIdx = useTrackIndex ? ev->track : ev->channel;

    switch (ev->type) {
    case 0x8: /* Note Off */
        m4a_engine_note_off(engine, trackIdx, ev->data0);
        break;
    case 0x9: /* Note On */
        m4a_engine_note_on(engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xB: /* Control Change */
        m4a_engine_cc(engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xC: /* Program Change */
        m4a_engine_program_change(engine, trackIdx, ev->data0);
        break;
    case 0xE: /* Pitch Bend — convert MIDI 14-bit unsigned to signed -8192..+8191 */
    {
        int16_t bend = (int16_t)(((int)(ev->data1 << 7) | ev->data0) - 8192);
        m4a_engine_pitch_bend(engine, trackIdx, bend);
        break;
    }
    }
}

/* Render a block of frames, chunked to fit in int */
static void render_frames(M4AEngine *engine, float *outL, float *outR,
                           uint64_t startSample, uint64_t frameCount)
{
    uint64_t remaining = frameCount;
    uint64_t pos = startSample;
    while (remaining > 0) {
        int chunk = (remaining > 0x7FFFFFFF) ? 0x7FFFFFFF : (int)remaining;
        m4a_engine_process(engine, outL + pos, outR + pos, chunk);
        pos       += (uint64_t)chunk;
        remaining -= (uint64_t)chunk;
    }
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *projectRoot   = argv[1];
    const char *vgName        = argv[2];
    const char *midiPath      = NULL;
    const char *outputPath    = NULL;
    bool        doPlay        = false;
    int         songVolume    = 127;
    int         reverbAmount  = 0;
    bool        analogFilter  = false;
    int         maxChannels   = 5;
    int         sampleRateHz  = 44100;
    double      tailSeconds   = 3.0;
    int         loopCount     = 2;
    double      fadeoutSeconds = 5.0;
    double      totalDurSeconds = -1.0; /* -1 = not set */

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--midi") == 0 && i + 1 < argc) {
            midiPath = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (strcmp(argv[i], "--play") == 0) {
            doPlay = true;
        } else if (strcmp(argv[i], "--song-volume") == 0 && i + 1 < argc) {
            songVolume = atoi(argv[++i]);
            if (songVolume < 0)   songVolume = 0;
            if (songVolume > 127) songVolume = 127;
        } else if (strcmp(argv[i], "--reverb") == 0 && i + 1 < argc) {
            reverbAmount = atoi(argv[++i]);
            if (reverbAmount < 0)   reverbAmount = 0;
            if (reverbAmount > 127) reverbAmount = 127;
        } else if (strcmp(argv[i], "--analog-filter") == 0) {
            analogFilter = true;
        } else if (strcmp(argv[i], "--polyphony") == 0 && i + 1 < argc) {
            maxChannels = atoi(argv[++i]);
            if (maxChannels < 1) maxChannels = 1;
            if (maxChannels > MAX_PCM_CHANNELS) maxChannels = MAX_PCM_CHANNELS;
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            sampleRateHz = atoi(argv[++i]);
            if (sampleRateHz < 8000) sampleRateHz = 8000;
        } else if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tailSeconds = atof(argv[++i]);
            if (tailSeconds < 0.0) tailSeconds = 0.0;
        } else if (strcmp(argv[i], "--loop-count") == 0 && i + 1 < argc) {
            loopCount = atoi(argv[++i]);
            if (loopCount < 1) loopCount = 1;
        } else if (strcmp(argv[i], "--fadeout") == 0 && i + 1 < argc) {
            fadeoutSeconds = atof(argv[++i]);
            if (fadeoutSeconds < 0.0) fadeoutSeconds = 0.0;
        } else if (strcmp(argv[i], "--total-duration-seconds") == 0 && i + 1 < argc) {
            totalDurSeconds = atof(argv[++i]);
            if (totalDurSeconds < 0.0) totalDurSeconds = 0.0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!midiPath) {
        fprintf(stderr, "Error: --midi is required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (!outputPath && !doPlay) {
        fprintf(stderr, "Error: at least one of --output or --play is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    double sampleRate = (double)sampleRateHz;

    /* ---- Parse MIDI ---- */
    printf("Parsing MIDI file: %s\n", midiPath);
    fflush(stdout);

    MidiTimeline *timeline = midi_timeline_load(midiPath, sampleRate);
    if (!timeline) return 1;

    /* For Type 1 MIDI files, use SMF track numbers as engine track indices.
     * This handles files where multiple tracks share the same MIDI channel
     * (each track gets its own program/voice in the M4A engine). */
    int useTrackIndex = (timeline->midiFormat == 1);

    printf("  %d MIDI events, raw duration: %.2f s\n",
           timeline->count, (double)timeline->totalMidiSamples / sampleRate);

    /* ---- Determine render plan ---- */
    uint64_t loopStartSample = timeline->loopStartSample;
    uint64_t loopEndSample = timeline->loopEndSample;
    bool hasLoop = (loopStartSample != UINT64_MAX &&
                    loopEndSample != UINT64_MAX &&
                    loopEndSample > loopStartSample);

    if (!hasLoop && (loopStartSample != UINT64_MAX || loopEndSample != UINT64_MAX))
        fprintf(stderr, "Warning: incomplete loop markers (need both '[' and ']' "
                        "text events with loop end > loop start)\n");

    uint64_t      totalSamples;
    uint64_t      fadeStartSample = UINT64_MAX; /* UINT64_MAX = no fadeout */
    const MidiRenderEvent *renderEvts;
    int            renderEvtCount;
    MidiRenderEvent *extEvts = NULL; /* allocated when loop is active */

    if (hasLoop) {
        uint64_t loopDuration  = loopEndSample - loopStartSample;
        uint64_t fadeoutSamps  = (uint64_t)(fadeoutSeconds * sampleRate + 0.5);

        if (totalDurSeconds >= 0.0) {
            totalSamples    = (uint64_t)(totalDurSeconds * sampleRate + 0.5);
            fadeStartSample = totalSamples > fadeoutSamps
                              ? totalSamples - fadeoutSamps : 0;
        } else {
            fadeStartSample = loopStartSample + (uint64_t)loopCount * loopDuration;
            totalSamples    = fadeStartSample + fadeoutSamps;
        }

        printf("  Loop region: [%.3f s, %.3f s] (%.3f s body)\n",
               (double)loopStartSample / sampleRate,
               (double)loopEndSample   / sampleRate,
               (double)loopDuration    / sampleRate);
        printf("  Fadeout: starts %.3f s, duration %.2f s\n",
               (double)fadeStartSample / sampleRate, fadeoutSeconds);

        /* Build extended event list:
         *   1. Pre-loop events (samplePos < loopStartSample) — played once.
         *   2. Loop body events (loopStartSample <= samplePos <= loopEndSample),
         *      repeated with increasing sample offsets until totalSamples.
         *
         * Iteration k has offset = k * loopDuration:
         *   iter 0 starts at loopStartSample  (original positions)
         *   iter 1 starts at loopEndSample     (seamless continuation)
         *   etc.
         *
         * Within each iteration events are added in original sorted order, so
         * note-offs at the loop boundary naturally precede the note-ons of the
         * next iteration at the same sample position.
         */
        int extCap   = timeline->count + 256;
        int extCount = 0;
        extEvts = malloc((size_t)extCap * sizeof(MidiRenderEvent));
        if (!extEvts) {
            fprintf(stderr, "Out of memory building event list\n");
            midi_timeline_free(timeline);
            return 1;
        }

        /* Pre-loop */
        for (int i = 0; i < timeline->count; i++) {
            if (timeline->events[i].samplePos < loopStartSample) {
                if (extCount >= extCap) {
                    extCap = extCap * 2 + 16;
                    MidiRenderEvent *p = realloc(extEvts, (size_t)extCap * sizeof(MidiRenderEvent));
                    if (!p) { free(extEvts); extEvts = NULL; goto oom; }
                    extEvts = p;
                }
                extEvts[extCount++] = timeline->events[i];
            }
        }

        /* Loop body iterations */
        if (loopDuration > 0) {
            for (uint64_t off = 0; loopStartSample + off < totalSamples; off += loopDuration) {
                for (int i = 0; i < timeline->count; i++) {
                    uint64_t op = timeline->events[i].samplePos;
                    if (op < loopStartSample || op > loopEndSample) continue;
                    uint64_t sp = op + off;
                    if (sp >= totalSamples) continue;
                    if (extCount >= extCap) {
                        extCap = extCap * 2 + 16;
                        MidiRenderEvent *p = realloc(extEvts, (size_t)extCap * sizeof(MidiRenderEvent));
                        if (!p) { free(extEvts); extEvts = NULL; goto oom; }
                        extEvts = p;
                    }
                    MidiRenderEvent ev = timeline->events[i];
                    ev.samplePos = sp;
                    extEvts[extCount++] = ev;
                }
            }
        }

        renderEvts      = extEvts;
        renderEvtCount  = extCount;
    } else {
        /* No loop: use original events + tail silence */
        uint64_t tailSamps = (uint64_t)(tailSeconds * sampleRate + 0.5);
        totalSamples       = timeline->totalMidiSamples + tailSamps;
        renderEvts         = timeline->events;
        renderEvtCount     = timeline->count;
    }

    printf("  Total render: %.2f s (%llu samples)\n",
           (double)totalSamples / sampleRate,
           (unsigned long long)totalSamples);

    if (0) {
oom:
        fprintf(stderr, "Out of memory building event list\n");
        midi_timeline_free(timeline);
        return 1;
    }

    /* ---- Load voicegroup ---- */
    printf("Loading voicegroup '%s' from %s...\n", vgName, projectRoot);
    fflush(stdout);

    LoadedVoiceGroup *vg = voicegroup_load(projectRoot, vgName, NULL);
    if (!vg) {
        fprintf(stderr, "Failed to load voicegroup '%s'\n", vgName);
        free(extEvts);
        midi_timeline_free(timeline);
        return 1;
    }
    printf("Voicegroup loaded successfully.\n");

    /* ---- Initialize engine ---- */
    M4AEngine engine;
    m4a_engine_init(&engine, (float)sampleRate);
    m4a_engine_set_voicegroup(&engine, vg->voices);
    m4a_engine_set_song_volume(&engine, (uint8_t)songVolume);
    m4a_reverb_set_amount(&engine.reverb, (uint8_t)reverbAmount);
    engine.analogFilter = analogFilter;
    engine.maxPcmChannels = (uint8_t)maxChannels;

    /* ---- Allocate output buffers ---- */
    float *outL = calloc(totalSamples, sizeof(float));
    float *outR = calloc(totalSamples, sizeof(float));
    if (!outL || !outR) {
        fprintf(stderr, "Out of memory allocating audio buffers (%llu samples)\n",
                (unsigned long long)totalSamples);
        free(outL); free(outR);
        m4a_engine_destroy(&engine);
        voicegroup_free(vg);
        free(extEvts);
        midi_timeline_free(timeline);
        return 1;
    }

    /* ---- Rendering loop ---- */
    printf("Rendering...\n");
    fflush(stdout);

    uint64_t samplePos = 0;
    for (int i = 0; i < renderEvtCount; i++) {
        const MidiRenderEvent *ev = &renderEvts[i];

        if (ev->samplePos >= totalSamples) break; /* safety: don't write past buffer */

        /* Render audio up to this event */
        if (ev->samplePos > samplePos)
            render_frames(&engine, outL, outR, samplePos,
                          ev->samplePos - samplePos);

        samplePos = ev->samplePos;
        dispatch_event(&engine, ev, useTrackIndex);
    }

    /* Render remaining frames (tail / fadeout section) */
    if (samplePos < totalSamples)
        render_frames(&engine, outL, outR, samplePos, totalSamples - samplePos);

    /* ---- Apply fadeout envelope ---- */
    if (fadeStartSample != UINT64_MAX && fadeStartSample < totalSamples) {
        uint64_t fadeSamps = totalSamples - fadeStartSample;
        for (uint64_t i = 0; i < fadeSamps; i++) {
            float gain = 1.0f - (float)i / (float)fadeSamps;
            outL[fadeStartSample + i] *= gain;
            outR[fadeStartSample + i] *= gain;
        }
    }

    printf("Rendering complete.\n");

    /* ---- WAV output ---- */
    if (outputPath) {
        printf("Writing %s...\n", outputPath);
        if (write_wav(outputPath, outL, outR, totalSamples, sampleRateHz) == 0)
            printf("Done: %s\n", outputPath);
    }

    /* ---- Speaker playback via miniaudio ---- */
    if (doPlay) {
        printf("Playing audio...\n");
        fflush(stdout);

#ifdef __linux__
        /* Suppress ALSA's verbose "cannot find card" error spam.  On WSL
         * there is no ALSA hardware, so miniaudio will fall back to
         * PulseAudio (provided by WSLg on Windows 11). */
        suppress_alsa_errors();

        /* Try PulseAudio before ALSA so that WSLg's PulseAudio server is
         * found without probing ALSA at all. */
        ma_backend linuxBackends[] = { ma_backend_pulseaudio, ma_backend_alsa };
        ma_context context;
        bool hasContext = (ma_context_init(linuxBackends, 2, NULL, &context) == MA_SUCCESS);
#endif

        PlaybackCtx ctx = { outL, outR, totalSamples, 0 };

        ma_device_config cfg  = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate        = (ma_uint32)sampleRateHz;
        cfg.dataCallback      = playback_callback;
        cfg.pUserData         = &ctx;

        ma_device device;
#ifdef __linux__
        ma_result initResult = ma_device_init(hasContext ? &context : NULL, &cfg, &device);
#else
        ma_result initResult = ma_device_init(NULL, &cfg, &device);
#endif
        if (initResult != MA_SUCCESS) {
            fprintf(stderr, "Failed to initialize audio playback device.\n");
#ifdef __linux__
            fprintf(stderr, "On WSL, audio requires PulseAudio (WSLg on Windows 11 provides this).\n");
#endif
        } else {
            if (ma_device_start(&device) != MA_SUCCESS) {
                fprintf(stderr, "Failed to start audio playback device\n");
            } else {
                while (ctx.pos < ctx.total)
                    ma_sleep(100);
            }
            ma_device_uninit(&device);
        }

#ifdef __linux__
        if (hasContext)
            ma_context_uninit(&context);
#endif

        printf("Playback complete.\n");
    }

    /* ---- Cleanup ---- */
    free(outL);
    free(outR);
    m4a_engine_destroy(&engine);
    voicegroup_free(vg);
    free(extEvts);
    midi_timeline_free(timeline);

    return 0;
}
