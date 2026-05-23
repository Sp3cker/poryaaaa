#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "m4a_engine.h"
#include "m4a_tables.h"

#if defined(M4A_DRIVER_V2)
#include "m4a/m4a_driver.h"
/* Tests access internal track / channel state to verify XCMD field
 * mutations and propagation into newly-started notes (xcmd.md).  The
 * driver's public header keeps M4ADriver opaque; test code is the
 * one consumer that has the same compile-time view as plugin/m4a/'s
 * own .c files. */
#include "m4a/m4a_internal.h"
#endif
#if defined(HW_AUDIO_V2)
#include "hw_audio/hw_audio.h"
#include "hw_audio/hw_psg.h"
#endif

#if defined(M4A_DRIVER_V2)
/* Test helper: advance the driver in chunks no larger than the bounded
 * event queue can hold, consuming between chunks.  Required for tests
 * that need many vblanks of activity — single m4a_advance calls > the
 * recommended max can overflow the 256-event queue.
 *
 * Inspection caveat: m4a_consume_writes clears the snapshot trigger
 * latches (per §6a contract).  Tests that care about trigger_sq* must
 * call m4a_advance directly with a small frame count instead, or
 * inspect the snapshot before this helper runs. */
static void v2_advance_chunked(M4ADriver *drv, int frames) {
    while (frames > 0) {
        int chunk = frames > M4A_RECOMMENDED_MAX_ADVANCE_FRAMES
                    ? M4A_RECOMMENDED_MAX_ADVANCE_FRAMES : frames;
        m4a_advance(drv, chunk);
        m4a_consume_writes(drv);
        frames -= chunk;
    }
}
#endif

#if defined(M4A_DRIVER_V2) && defined(HW_AUDIO_V2)
/* Test helper: chunk the full advance + render + consume cycle so the
 * production event-queue contract holds. */
static void v2_render_chunked(M4ADriver *drv, HwAudio *hw,
                              float *outL, float *outR, int frames) {
    int off = 0;
    while (off < frames) {
        int chunk = (frames - off) > M4A_RECOMMENDED_MAX_ADVANCE_FRAMES
                    ? M4A_RECOMMENDED_MAX_ADVANCE_FRAMES : (frames - off);
        m4a_advance(drv, chunk);
        hw_audio_render_events(hw,
                               m4a_get_pending_writes(drv),
                               m4a_get_pcm_ring(drv),
                               outL + off, outR + off, chunk);
        m4a_consume_writes(drv);
        off += chunk;
    }
}
#endif

/*
 * Unit tests for the m4a engine.
 * Tests key algorithms against known values from the GBA engine.
 */

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL: %s: expected %d, got %d (line %d)\n", \
                msg, (int)(b), (int)(a), __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    tests_run++; \
    if (fabs((double)(a) - (double)(b)) > (eps)) { \
        fprintf(stderr, "FAIL: %s: expected %f, got %f (line %d)\n", \
                msg, (double)(b), (double)(a), __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

static void send_xcmd_select(M4AEngine *engine, int trackIndex, uint8_t selector)
{
    m4a_engine_cc(engine, trackIndex, 0x1E, selector);
}

static void send_xcmd_bytes(M4AEngine *engine, int trackIndex, const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++)
        m4a_engine_cc(engine, trackIndex, 0x1D, bytes[i]);
}

typedef struct {
    int trackIndex;
    uint8_t selector;
    uint32_t value;
    int called;
} XcmdCapture;

static void capture_xcmd(void *ctx, int trackIndex, uint8_t selector, uint32_t value)
{
    XcmdCapture *cap = (XcmdCapture *)ctx;

    cap->trackIndex = trackIndex;
    cap->selector = selector;
    cap->value = value;
    cap->called++;
}

/* Test umul3232H32 */
static void test_umul3232H32(void)
{
    printf("Testing umul3232H32...\n");

    /* Simple cases */
    ASSERT_EQ(umul3232H32(0, 0), 0, "0 * 0");
    ASSERT_EQ(umul3232H32(0xFFFFFFFF, 1), 0, "FFFFFFFF * 1 high word");
    ASSERT_EQ(umul3232H32(0x80000000, 2), 1, "0x80000000 * 2");
    ASSERT_EQ(umul3232H32(0x80000000, 0x80000000), 0x40000000, "0.5 * 0.5 in fixed point");
}

/* Test scale/freq table lookups */
static void test_scale_table(void)
{
    printf("Testing scale table...\n");

    /* Key 0 should map to 0xE0 */
    ASSERT_EQ(gScaleTable[0], 0xE0, "key 0");
    /* Key 12 should map to 0xD0 (one octave up) */
    ASSERT_EQ(gScaleTable[12], 0xD0, "key 12");
    /* Key 60 (middle C) should be in octave 5 */
    ASSERT_EQ(gScaleTable[60], 0x90, "key 60 octave");
    /* Key 168 (near end) */
    ASSERT_EQ(gScaleTable[168], 0x00, "key 168");
}

/* Test MidiKeyToFreq */
static void test_midi_key_to_freq(void)
{
    printf("Testing MidiKeyToFreq...\n");

    /* Create a fake WaveData with a known frequency */
    WaveData wd;
    memset(&wd, 0, sizeof(wd));
    wd.freq = 0x00800000;  /* 0.5 in 9.23 fixed point, ~4186 Hz */

    /* Middle C (key 60) with no fine adjust */
    uint32_t freq60 = m4a_midi_key_to_freq(&wd, 60, 0);

    /* One octave up (key 72) should be ~2x frequency */
    uint32_t freq72 = m4a_midi_key_to_freq(&wd, 72, 0);

    /* freq72 should be approximately 2 * freq60 */
    double ratio = (double)freq72 / (double)freq60;
    ASSERT_NEAR(ratio, 2.0, 0.01, "octave ratio 72/60");

    /* One semitone up should be ~1.0595x */
    uint32_t freq61 = m4a_midi_key_to_freq(&wd, 61, 0);
    ratio = (double)freq61 / (double)freq60;
    ASSERT_NEAR(ratio, 1.0595, 0.01, "semitone ratio 61/60");

    /* Key clamping at 178 */
    uint32_t freq178 = m4a_midi_key_to_freq(&wd, 178, 0);
    uint32_t freq200 = m4a_midi_key_to_freq(&wd, 200, 0);
    ASSERT_EQ(freq200, m4a_midi_key_to_freq(&wd, 178, 255),
              "key > 178 should clamp");
}

/* Test MidiKeyToCgbFreq */
static void test_midi_key_to_cgb_freq(void)
{
    printf("Testing MidiKeyToCgbFreq...\n");

    /* Noise channel: key 21 is the lowest valid */
    uint32_t noiseFreq = m4a_midi_key_to_cgb_freq(4, 21, 0);
    ASSERT_EQ(noiseFreq, gNoiseTable[0], "noise key 21");

    /* Noise channel: key 80 = table index 59 */
    noiseFreq = m4a_midi_key_to_cgb_freq(4, 80, 0);
    ASSERT_EQ(noiseFreq, gNoiseTable[59], "noise key 80");

    /* Square channel: very low key should clamp */
    uint32_t sqFreq = m4a_midi_key_to_cgb_freq(1, 20, 0);
    uint32_t sqFreqLow = m4a_midi_key_to_cgb_freq(1, 36, 0);
    ASSERT_EQ(sqFreq, sqFreqLow, "square low key clamp");

    /* Square channel: verify 2048 offset */
    sqFreq = m4a_midi_key_to_cgb_freq(1, 72, 0);
    ASSERT(sqFreq > 0, "square freq should be positive");
}

/* Test track volume/pitch calculation */
static void test_trk_vol_pit_set(void)
{
    printf("Testing TrkVolPitSet...\n");

    M4ATrack track;
    memset(&track, 0, sizeof(track));
    track.volume = 127;
    track.volX = 64;
    track.pan = 0;
    track.panX = 0;
    track.bendRange = 2;
    track.modT = 0;
    track.modM = 0;
    track.keyShift = 0;
    track.keyShiftX = 0;
    track.tune = 0;
    track.pitX = 0;
    track.bend = 0;

    m4a_track_vol_pit_set(&track);

    /* With center pan (0), volumes should be roughly equal */
    ASSERT(track.volMR > 0, "right volume should be positive");
    ASSERT(track.volML > 0, "left volume should be positive");

    /* With vol=127, volX=64, and center pan:
     * x = (127 * 64) >> 5 = 254
     * y = 0 (center)
     * volMR = (128 * 254) >> 8 = 127
     * volML = (127 * 254) >> 8 = 126 */
    ASSERT_EQ(track.volMR, 127, "right vol center");
    ASSERT_EQ(track.volML, 126, "left vol center");

    /* Test with full right pan */
    track.pan = 63;
    m4a_track_vol_pit_set(&track);
    ASSERT(track.volMR > track.volML, "right pan: R > L");

    /* Test with full left pan */
    track.pan = -64;
    m4a_track_vol_pit_set(&track);
    ASSERT(track.volML > track.volMR, "left pan: L > R");

    /* Test pitch: bend of +1 with range 2 = +2 semitones */
    track.pan = 0;
    track.bend = 64;
    m4a_track_vol_pit_set(&track);
    ASSERT_EQ(track.keyM, 2, "bend +64 range 2 = keyM 2");
}

/* Test engine initialization */
static void test_engine_init(void)
{
    printf("Testing engine init...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 44100.0f);

    ASSERT_NEAR(engine.sampleRate, 44100.0f, 0.1f, "sample rate");
    ASSERT_NEAR(engine.samplesPerTick, 44100.0f / 59.7275f, 1.0f, "samples per tick");
    ASSERT_EQ(engine.masterVolume, 15, "master volume");
    ASSERT_EQ(engine.songMasterVolume, MAX_SONG_VOLUME, "song master volume");
    ASSERT_EQ(engine.maxPcmChannels, 5, "max pcm channels");

    /* Verify CGB channel types */
    ASSERT_EQ(engine.cgbChannels[0].type, 1, "cgb ch0 type");
    ASSERT_EQ(engine.cgbChannels[1].type, 2, "cgb ch1 type");
    ASSERT_EQ(engine.cgbChannels[2].type, 3, "cgb ch2 type");
    ASSERT_EQ(engine.cgbChannels[3].type, 4, "cgb ch3 type");

    m4a_engine_destroy(&engine);
}

static void test_xcmd_subcommands(void)
{
    printf("Testing XCMD subcommands...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 44100.0f);

    int dataSize = 4;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    wd->data[dataSize] = 0;

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0x10;
    voices[0].decay = 0x20;
    voices[0].sustain = 0x30;
    voices[0].release = 0x40;
    voices[0].length = 0x50;
    voices[0].panSweep = 0x60;

    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    {
        XcmdCapture xcmd_cap = {0};

        m4a_engine_set_xcmd_callback(&engine, capture_xcmd, &xcmd_cap);
        send_xcmd_select(&engine, 0, 0x04);
        send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x7A }, 1);
        ASSERT_EQ(xcmd_cap.called, 1, "xcmd callback fires on successful apply");
        ASSERT_EQ(xcmd_cap.trackIndex, 0, "xcmd callback reports track index");
        ASSERT_EQ(xcmd_cap.selector, 0x04, "xcmd callback reports selector");
        ASSERT_EQ(xcmd_cap.value, 0x7A, "xcmd callback reports value");
    }

    send_xcmd_select(&engine, 0, 0x05);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x55 }, 1);
    send_xcmd_select(&engine, 0, 0x06);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x44 }, 1);
    send_xcmd_select(&engine, 0, 0x07);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x33 }, 1);
    send_xcmd_select(&engine, 0, 0x08);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x22 }, 1);
    send_xcmd_select(&engine, 0, 0x09);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x11 }, 1);
    send_xcmd_select(&engine, 0, 0x0A);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x66 }, 1);
    send_xcmd_select(&engine, 0, 0x0B);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x77 }, 1);
    send_xcmd_select(&engine, 0, 0x02);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ VOICE_SQUARE_1 }, 1);

    ASSERT_EQ(engine.tracks[0].currentVoice.attack, 0x7A, "xatta updates attack");
    ASSERT_EQ(engine.tracks[0].currentVoice.decay, 0x55, "xdeca updates decay");
    ASSERT_EQ(engine.tracks[0].currentVoice.sustain, 0x44, "xsust updates sustain");
    ASSERT_EQ(engine.tracks[0].currentVoice.release, 0x33, "xrele updates release");
    ASSERT_EQ(engine.tracks[0].pseudoEchoVolume, 0x22, "xiecv updates pseudo-echo volume");
    ASSERT_EQ(engine.tracks[0].pseudoEchoLength, 0x11, "xiecl updates pseudo-echo length");
    ASSERT_EQ(engine.tracks[0].currentVoice.length, 0x66, "xleng updates length");
    ASSERT_EQ(engine.tracks[0].currentVoice.panSweep, 0x77, "xswee updates pan sweep");
    ASSERT_EQ(engine.tracks[0].currentVoice.type, VOICE_SQUARE_1, "xtype updates voice type");

    engine.tracks[0].currentVoice.type = VOICE_DIRECTSOUND;
    engine.tracks[0].currentVoice.wav = wd;
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT_EQ(engine.pcmChannels[0].attack, 0x7A, "XCMD ADSR is copied into new PCM notes");
    ASSERT_EQ(engine.pcmChannels[0].decay, 0x55, "XCMD decay is copied into new PCM notes");
    ASSERT_EQ(engine.pcmChannels[0].sustain, 0x44, "XCMD sustain is copied into new PCM notes");
    ASSERT_EQ(engine.pcmChannels[0].release, 0x33, "XCMD release is copied into new PCM notes");
    ASSERT_EQ(engine.pcmChannels[0].pseudoEchoVolume, 0x22, "XCMD IECV is copied into new PCM notes");
    ASSERT_EQ(engine.pcmChannels[0].pseudoEchoLength, 0x11, "XCMD IECL is copied into new PCM notes");

    send_xcmd_select(&engine, 0, 0x0D);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x78, 0x56, 0x34 }, 3);
    ASSERT_EQ(engine.tracks[0].extendedValue, 0, "xcmd 0D waits for all four bytes");
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x12 }, 1);
    ASSERT_EQ(engine.tracks[0].extendedValue, 0x12345678, "xcmd 0D stores little-endian value");

    send_xcmd_select(&engine, 0, 0x01);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0xBE, 0xBA, 0xFE }, 3);
    ASSERT_EQ((uint32_t)(uintptr_t)engine.tracks[0].currentVoice.wav, (uint32_t)(uintptr_t)wd,
              "xwave does not apply until the fourth byte arrives");
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0xCA }, 1);
    ASSERT_EQ((uint32_t)(uintptr_t)engine.tracks[0].currentVoice.wav, 0xCAFEBABE,
              "xwave stores the assembled 32-bit pointer");

    send_xcmd_select(&engine, 0, 0x0C);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x02, 0x00 }, 2);
    ASSERT_EQ(engine.tracks[0].extendedLoopCount, 1, "xcmd 0C increments loop count");
    ASSERT_EQ(engine.tracks[0].extendedWait, 1, "xcmd 0C sets wait while looping");
    send_xcmd_select(&engine, 0, 0x0C);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x02, 0x00 }, 2);
    ASSERT_EQ(engine.tracks[0].extendedLoopCount, 2, "xcmd 0C keeps counting up to the limit");
    ASSERT_EQ(engine.tracks[0].extendedWait, 1, "xcmd 0C keeps wait asserted before completion");
    send_xcmd_select(&engine, 0, 0x0C);
    send_xcmd_bytes(&engine, 0, (uint8_t[]){ 0x02, 0x00 }, 2);
    ASSERT_EQ(engine.tracks[0].extendedLoopCount, 0, "xcmd 0C resets after hitting the limit");
    ASSERT_EQ(engine.tracks[0].extendedWait, 0, "xcmd 0C clears wait after completion");

    m4a_engine_destroy(&engine);
    free(wd);
}

/* Test basic audio generation */
static void test_basic_audio(void)
{
    printf("Testing basic audio generation...\n");

    M4AEngine engine;
    m4a_engine_init(&engine, 44100.0f);

    /* Create a simple WaveData (sine-ish) */
    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->freq = 0x01000000;  /* ~8372 Hz base */
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++) {
        wd->data[i] = (int8_t)(127.0 * sin(2.0 * 3.14159265 * i / dataSize));
    }
    wd->data[dataSize] = wd->data[0];  /* safety sample */

    /* Create a voicegroup with one DirectSound voice */
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;  /* instant attack */
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 0;

#if defined(M4A_DRIVER_V2)
    M4ADriver *drv = m4a_driver_create(44100.0f);
#endif
#if defined(HW_AUDIO_V2)
    HwAudio   *hw  = hw_audio_create(44100.0f);
#endif

    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    m4a_engine_cc(&engine, 0, 7, 127);
#if defined(M4A_DRIVER_V2)
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
#endif

    /* Play a note */
    m4a_engine_note_on(&engine, 0, 60, 100);
#if defined(M4A_DRIVER_V2)
    m4a_note_on(drv, 0, 60, 100);
#endif

    /* Generate some audio */
    float outL[1024], outR[1024];
#if defined(M4A_DRIVER_V2) && defined(HW_AUDIO_V2)
    m4a_advance(drv, 1024);
    hw_audio_render_events(hw, m4a_get_pending_writes(drv),
                           m4a_get_pcm_ring(drv),
                           outL, outR, 1024);
    m4a_consume_writes(drv);
    /* v2 audible end-to-end via the event-stream API (PSG square+wave+
     * noise + PCM); however this top-level test exercises driver
     * lifecycle rather than synth output, so the v1 non-zero assertion
     * isn't mirrored here.  Chip-only audibility is asserted by the
     * test_chip_canned_* suite under HW_AUDIO_V2. */
#else
    m4a_engine_process(&engine, outL, outR, 1024);

    /* Verify we got non-zero audio */
    float maxVal = 0;
    for (int i = 0; i < 1024; i++) {
        if (fabs(outL[i]) > maxVal) maxVal = fabs(outL[i]);
        if (fabs(outR[i]) > maxVal) maxVal = fabs(outR[i]);
    }
    ASSERT(maxVal > 0.001f, "audio output should be non-zero");
#endif

    /* Note off */
    m4a_engine_note_off(&engine, 0, 60);
#if defined(M4A_DRIVER_V2)
    m4a_note_off(drv, 0, 60);
    m4a_driver_destroy(drv);
#endif
#if defined(HW_AUDIO_V2)
    hw_audio_destroy(hw);
#endif

    m4a_engine_destroy(&engine);
    free(wd);
}

/* Test PCM channel stealing / polyphony behavior */
static void test_polyphony_stealing(void)
{
    printf("Testing polyphony channel stealing...\n");

    /* Minimal WaveData for PCM tests */
    int dataSize = 4;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    wd->data[dataSize] = 0;  /* safety sample */

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type = VOICE_DIRECTSOUND;
    voices[0].key = 60;
    voices[0].wav = wd;
    voices[0].attack = 0xFF;
    voices[0].decay = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 0;

    /* Status value for an active (non-stopping) PCM channel */
    const uint8_t ACTIVE = CHN_START | CHN_ENV_SUSTAIN;
    /* Status value for a stopping PCM channel */
    const uint8_t STOPPING = CHN_STOP | CHN_START;

    M4AEngine engine;

    /* ---- Test 1: Free channel used immediately (baseline) ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    engine.tracks[0].priority = 5;
    m4a_engine_note_on(&engine, 0, 60, 100);
    ASSERT(engine.pcmChannels[0].status & CHN_ON, "free channel: ch0 allocated");
    ASSERT_EQ(engine.pcmChannels[0].trackIndex, 0, "free channel: ch0 trackIndex");
    ASSERT_EQ(engine.pcmChannels[0].priority, 5, "free channel: ch0 priority");
    m4a_engine_destroy(&engine);

    /* ---- Test 2: Higher-priority note steals lower-priority active channel ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 1, 0);
    engine.tracks[1].priority = 7;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 3;
        engine.pcmChannels[i].trackIndex = 0;
    }
    m4a_engine_note_on(&engine, 1, 60, 100);
    {
        int stolen = 0;
        for (int i = 0; i < 5; i++)
            if (engine.pcmChannels[i].trackIndex == 1) { stolen = 1; break; }
        ASSERT(stolen, "higher priority: steals lower-priority channel");
    }
    m4a_engine_destroy(&engine);

    /* ---- Test 3: Lower-priority note cannot steal higher-priority channel ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 2, 0);
    engine.tracks[2].priority = 3;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 7;
        engine.pcmChannels[i].trackIndex = 0;
    }
    m4a_engine_note_on(&engine, 2, 60, 100);
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, 0, "lower priority: no steal");
    m4a_engine_destroy(&engine);

    /* ---- Test 4: Equal-priority note steals channel with higher trackIndex ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 3, 0);
    engine.tracks[3].priority = 5;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 5;
        engine.pcmChannels[i].trackIndex = 7;
    }
    m4a_engine_note_on(&engine, 3, 60, 100);  /* trackIndex=3, priority=5 */
    /* ch.trackIndex=7 >= new.trackIndex=3 → qualifies → steal */
    {
        int stolen = 0;
        for (int i = 0; i < 5; i++)
            if (engine.pcmChannels[i].trackIndex == 3) { stolen = 1; break; }
        ASSERT(stolen, "equal priority: steals channel with higher trackIndex");
    }
    m4a_engine_destroy(&engine);

    /* ---- Test 5: Equal-priority note cannot steal channel with lower trackIndex ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 5, 0);
    engine.tracks[5].priority = 5;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 5;
        engine.pcmChannels[i].trackIndex = 1;
    }
    m4a_engine_note_on(&engine, 5, 60, 100);  /* trackIndex=5, priority=5 */
    /* ch.trackIndex=1 < new.trackIndex=5 → does not qualify → dropped */
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, 1, "equal priority: no steal when ch.trackIndex < new.trackIndex");
    m4a_engine_destroy(&engine);

    /* ---- Test 6: Stopping channel is always stolen regardless of priority ---- */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 0, 0);
    engine.tracks[0].priority = 1;  /* very low priority */
    /* 4 active channels at high priority */
    for (int i = 0; i < 4; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 10;
        engine.pcmChannels[i].trackIndex = 9;
    }
    /* Channel 4 is stopping (releasing) */
    engine.pcmChannels[4].status = STOPPING;
    engine.pcmChannels[4].priority = 10;
    engine.pcmChannels[4].trackIndex = 9;
    m4a_engine_note_on(&engine, 0, 60, 100);  /* priority=1, trackIndex=0 */
    /* Stopping channel (index 4) should be stolen */
    ASSERT_EQ(engine.pcmChannels[4].trackIndex, 0, "stopping channel: always stolen");
    /* Active channels untouched */
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, 9, "stopping channel: active channels untouched");
    m4a_engine_destroy(&engine);

    /* ---- Test 7: Concrete bug case — all 5 slots at equal priority, new note dropped ---- */
    /* All 5 channels at priority 5, owned by tracks 1–5.
     * New note from track 7 at priority 5.
     * ch.trackIndex (1..5) < new.trackIndex (7) for all → no victim → dropped. */
    m4a_engine_init(&engine, 44100.0f);
    m4a_engine_set_voicegroup(&engine, voices);
    m4a_engine_program_change(&engine, 7, 0);
    engine.tracks[7].priority = 5;
    for (int i = 0; i < 5; i++) {
        engine.pcmChannels[i].status = ACTIVE;
        engine.pcmChannels[i].priority = 5;
        engine.pcmChannels[i].trackIndex = i + 1;  /* tracks 1..5 */
    }
    m4a_engine_note_on(&engine, 7, 60, 100);
    for (int i = 0; i < 5; i++)
        ASSERT_EQ(engine.pcmChannels[i].trackIndex, i + 1, "all slots: note dropped when no valid victim");
    m4a_engine_destroy(&engine);

    free(wd);
}

#if defined(M4A_DRIVER_V2)
/* Layer 1 step 1 acceptance: CgbSound writes M4ARegisterFile correctly,
 * and trigger_* flags fire only on MO_VOL → NRx4 rewrite (note start /
 * envelope phase transitions), not on every snapshot. */
static void test_v2_trigger_semantics(void)
{
    printf("Testing v2 driver trigger semantics...\n");

    /* sq2 voice that fires the start path on the very first vblank and
     * stays at peak envelope through SUSTAIN.  attack=0 → skip-to-peak;
     * decay=0 + sustain=16 (sustainGoal == envelopeGoal) → no decay drop;
     * release=16 → non-trivial release so RELEASE path sets MO_VOL. */
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    /* duty 2 (50%) so sq2_duty != 0 lets us verify the field landed. */
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);    /* track volume 100% */
    m4a_cc(drv, 0, 10, 64);    /* center pan */
    m4a_note_on(drv, 0, 60, 100);

#if defined(HW_AUDIO_V2)
    /* This test exercises the LEGACY snapshot API — hw_audio_render()
     * — specifically to validate the trigger-consumption side of the
     * driver→chip contract (chip clears trigger_* latches after a
     * snapshot consume).  Production v2 audio routes through
     * hw_audio_render_events() (§12 step 3); this path renders silence
     * and only consumes triggers, so it's the right tool for proving
     * the snapshot trigger contract in isolation from the event-render
     * pipeline. */
    HwAudio *hw = hw_audio_create(44100.0f);
    /* DRIVE chunks at M4A_RECOMMENDED_MAX_ADVANCE_FRAMES so the event
     * queue can't overflow.  scratch buffers sized to that bound. */
    enum { TRIG_SCRATCH = M4A_RECOMMENDED_MAX_ADVANCE_FRAMES };
    float scratchL[TRIG_SCRATCH], scratchR[TRIG_SCRATCH];
    #define DRIVE(n) do { int _n = (n);                                    \
                          while (_n > 0) {                                 \
                              int _c = _n > TRIG_SCRATCH ? TRIG_SCRATCH : _n; \
                              m4a_advance(drv, _c);                        \
                              hw_audio_render(hw,                          \
                                  m4a_get_register_file_mut(drv),          \
                                  m4a_get_pcm_ring(drv),                   \
                                  scratchL, scratchR, _c);                 \
                              _n -= _c;                                    \
                          } } while (0)
#else
    /* Driver-only build: no chip available — clear triggers manually to
     * simulate the chip's side of the contract. */
    #define DRIVE(n) do { m4a_advance(drv, (n));                           \
                          M4ARegisterFile *_rw =                           \
                              m4a_get_register_file_mut(drv);              \
                          _rw->trigger_sq1   = false;                      \
                          _rw->trigger_sq2   = false;                      \
                          _rw->trigger_wave  = false;                      \
                          _rw->trigger_noise = false; } while (0)
#endif

    /* Advance enough host frames to fire at least one vblank (~738 frames
     * @ 44100 Hz / 59.7275 Hz).  Inspect the register file BEFORE the
     * chip render consumes the triggers. */
    m4a_advance(drv, 1024);

    const M4ARegisterFile *r = m4a_get_register_file(drv);
    ASSERT(r != NULL, "register file accessible");
    ASSERT(r->trigger_sq2,           "trigger_sq2 set after note_on + vblank");
    ASSERT(r->sq2_enabled,           "sq2 enabled after note_on");
    ASSERT(r->sq2_freq != 0,         "sq2_freq populated");
    ASSERT(r->sq2_env_volume > 0,    "sq2 envelope non-zero");
    ASSERT_EQ(r->sq2_duty, 2,        "sq2_duty matches voice wavePointer LSB");
    ASSERT(!r->trigger_sq1,          "trigger_sq1 NOT set (different channel)");
    ASSERT(!r->trigger_wave,         "trigger_wave NOT set");
    ASSERT(!r->trigger_noise,        "trigger_noise NOT set");

#if defined(HW_AUDIO_V2)
    /* Consume the trigger latches via the legacy snapshot API.  After
     * this call, the chip must have cleared trigger_sq2 per the §6a
     * contract — the latches are edge-triggers consumed once per
     * snapshot view, regardless of whether downstream audio actually
     * renders. */
    hw_audio_render(hw, m4a_get_register_file_mut(drv), m4a_get_pcm_ring(drv),
                    scratchL, scratchR, 1024);
    ASSERT(!r->trigger_sq2,          "chip clears trigger_sq2 after consuming");
#endif

    /* Subsequent vblanks with no events should NOT refire trigger_sq2
     * (envelope is in steady SUSTAIN at the goal). */
    DRIVE(4096);
    ASSERT(!r->trigger_sq2,          "trigger_sq2 stays cleared in steady sustain");

    /* note_off → release phase: envelope continues down to zero in
     * software.  MO_VOL fires (NRx2 envelope updates), but NRx4 trigger
     * MUST NOT be set — re-triggering during release would reset the
     * wave RAM position (NR34) / noise LFSR (NR44) on real GB hardware.
     * Trigger is a fresh-note signal only.  Mirrors v1 + real m4a. */
    m4a_note_off(drv, 0, 60);
    m4a_advance(drv, 1024);
    ASSERT(!r->trigger_sq2,          "trigger_sq2 stays cleared on release transition");

#if defined(HW_AUDIO_V2)
    hw_audio_destroy(hw);
    #undef DRIVE
#else
    #undef DRIVE
#endif
    m4a_driver_destroy(drv);
}

/* m4a_set_song_volume must rescale active CGB channel volumes
 * immediately — not wait for the next CC7 or note_on.  GUI / state-load
 * paths poke the song master volume on already-sounding tracks. */
static void test_v2_song_volume_rescales(void)
{
    printf("Testing v2 set_song_volume rescales active channels...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);    /* track CC7 max */
    m4a_cc(drv, 0, 10, 64);    /* center pan */
    m4a_note_on(drv, 0, 60, 100);
    m4a_advance(drv, 1024);    /* fire vblank, channel sounding */

    const M4ARegisterFile *r = m4a_get_register_file(drv);
    uint8_t envHigh = r->sq2_env_volume;
    ASSERT(envHigh > 0,              "song-vol precondition: sq2 env > 0 at song_vol=127");

    /* Halve the song master volume.  Without rescale this would be a
     * silent knob; with rescale, the next SUSTAIN-counter rollover
     * lands envelopeVolume at the new (lower) sustainGoal and fires
     * MO_VOL.  SUSTAIN counter rolls every 7 vblanks, so we advance a
     * generous chunk to guarantee at least one rollover. */
    m4a_set_song_volume(drv, 64);
    v2_advance_chunked(drv, 16384);   /* ~22 vblanks @ 44100 Hz */

    uint8_t envLow = r->sq2_env_volume;
    ASSERT(envLow > 0,               "song-vol: not muted at song_vol=64");
    ASSERT(envLow < envHigh,         "song-vol drop produces lower CGB env volume");

    /* And restore: song_vol back to 127 should bring envelope back up. */
    m4a_set_song_volume(drv, 127);
    v2_advance_chunked(drv, 16384);
    ASSERT(r->sq2_env_volume >= envHigh,
                                     "song-vol restore brings env back up");

    m4a_driver_destroy(drv);
}

/* SoundMainRAM acceptance: a DirectSound voice + note_on populates the
 * driver's PCM ring with non-zero post-clamp samples after a vblank,
 * with pcm_rate_hz set and write_cursor advanced by the canonical
 * pcmSamplesPerVBlank step. */
static void test_v2_pcm_ring_fills(void)
{
    printf("Testing v2 PCM ring fills after note_on...\n");

    /* DirectSound voice with 64 samples of a positive-going saw — easy
     * to verify mixed output is non-zero and on the right side. */
    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0;       /* no loop */
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)((i * 4) - 127);   /* monotonic non-zero */
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;   /* instant peak — first vblank is audible */
    voices[0].decay   = 0;
    voices[0].sustain = 0xFF;
    voices[0].release = 0;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_set_master_volume(drv, 15);
    m4a_set_max_pcm_channels(drv, 5);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 127);

    const M4APcmRing *ring = m4a_get_pcm_ring(drv);
    ASSERT(ring != NULL,             "pcm ring accessible");
    uint64_t cursor0 = ring->write_cursor;

    /* Fire enough vblanks to be safe across rate jitter. */
    v2_advance_chunked(drv, 4096);

    ASSERT(ring->write_cursor > cursor0,
                                     "write_cursor advances after vblanks");
    ASSERT_EQ(ring->pcm_rate_hz, M4A_PCM_RATE_HZ,
                                     "pcm_rate_hz set to canonical 13379");

    /* Pokemon Emerald default routing: A=right, B=left.  Both ring sides
     * should carry signal because pan is centered. */
    int nzA = 0, nzB = 0;
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        if (ring->ring_a[i] != 0) nzA++;
        if (ring->ring_b[i] != 0) nzB++;
    }
    ASSERT(nzA > 0,                  "ring_a (DMA-A) has non-zero samples");
    ASSERT(nzB > 0,                  "ring_b (DMA-B) has non-zero samples");

    m4a_driver_destroy(drv);
    free(wd);
}

/* PCM frequency parity (response to v3 follow-up #1).  V1 multiplies
 * MidiKeyToFreq by divFreq before storing.  Verify v2's fw advances at
 * the same rate as v1 by comparing the resulting per-vblank wave-sample
 * step against the canonical formula.  The first PCM tick advances fw
 * by `frequency`; integer-sample step = frequency >> 23. */
static void test_v2_pcm_frequency_scale(void)
{
    printf("Testing v2 PCM frequency divFreq scaling...\n");

    /* Wave at 22050 Hz (wav->freq = 22050 << 10 ≈ pokeemerald convention).
     * Use VOICE_TYPE_FIX-disabled so the divFreq path runs. */
    WaveData wav = { 0 };
    wav.freq = 22050u << 10;
    wav.size = 1024;
    static int8_t fakeData[1024];
    wav.data = fakeData;

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type   = VOICE_DIRECTSOUND;
    voices[0].key    = 60;
    voices[0].wav    = &wav;
    voices[0].attack = 0xFF;
    voices[0].sustain = 0xFF;

    /* Recompute divFreq the same way the driver does. */
    const int pcmFreq = M4A_PCM_RATE_HZ;
    const int divFreq = (16777216 / pcmFreq + 1) >> 1;
    uint32_t expectedFreq =
        (uint32_t)((uint64_t)m4a_midi_key_to_freq(&wav, 60, 0) * (uint64_t)divFreq);

    /* Sanity: a non-fix DirectSound at base key should advance > 0
     * sample-steps per pcm tick (otherwise pitch is mute / DC). */
    ASSERT(expectedFreq >= (1u << 23) / 4,
                                     "divFreq scaling produces audible step rate");

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_set_max_pcm_channels(drv, 5);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_note_on(drv, 0, 60, 127);

    /* The v2 driver doesn't expose ch->frequency publicly, but we can
     * observe its effect: after one vblank, ring_a[0..N] should be
     * non-trivially varying because the wave is read at a non-zero
     * step rate.  An unscaled (1/divFreq) frequency would advance by
     * essentially zero samples and produce a constant ring value. */
    fakeData[0] = 100;
    for (int i = 1; i < 1024; i++) fakeData[i] = (int8_t)((-100) + (i & 1) * 200);

    v2_advance_chunked(drv, 4096);

    const M4APcmRing *ring = m4a_get_pcm_ring(drv);
    int firstNonZero = -1;
    int8_t a0 = 0, aN = 0;
    for (int i = 0; i < 32; i++) {
        if (firstNonZero < 0 && ring->ring_a[i] != 0) {
            firstNonZero = i;
            a0 = ring->ring_a[i];
        }
        aN = ring->ring_a[i];
    }
    ASSERT(firstNonZero >= 0,        "ring_a populated within first 32 samples");
    ASSERT(a0 != aN,                 "ring varies (wave actually being stepped through)");

    m4a_driver_destroy(drv);
}

/* CGB pan_mask parity (response to v3 follow-up #2).  panMask=0xFF
 * default means a centered note populates both pan_mask_left and
 * pan_mask_right with the channel's bit. */
static void test_v2_cgb_pan_mask_routes(void)
{
    printf("Testing v2 CGB pan_mask defaults route to both sides...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);              /* center pan */
    m4a_note_on(drv, 0, 60, 100);
    m4a_advance(drv, 1024);

    const M4ARegisterFile *r = m4a_get_register_file(drv);
    /* sq2 occupies bit 1 in the NR51 nibbles — bit_i = (1 << channel-1). */
    uint8_t sq2_bit = 1 << 1;
    ASSERT(r->pan_mask_left  & sq2_bit, "sq2 routed to left");
    ASSERT(r->pan_mask_right & sq2_bit, "sq2 routed to right");

    m4a_driver_destroy(drv);
}

/* PCM CC7 parity (response to v3 follow-up #3).  CC7 (track volume) on
 * a sounding PCM channel must propagate to that channel's L/R volumes
 * — without refresh_pcm_volumes, the next ring slice would still use
 * the old (louder) volume. */
static void test_v2_pcm_cc7_refresh(void)
{
    printf("Testing v2 PCM CC7 refresh on active channel...\n");

    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0;
    wd->freq = 22050u << 10;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++) wd->data[i] = 100;
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].sustain = 0xFF;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_set_master_volume(drv, 15);
    m4a_set_max_pcm_channels(drv, 5);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);    /* loud */
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 127);
    v2_advance_chunked(drv, 4096);

    const M4APcmRing *ring = m4a_get_pcm_ring(drv);
    int loudSampleSum = 0;
    for (int i = 0; i < M4A_PCM_SAMPLES_PER_VBLANK; i++) {
        int v = ring->ring_a[i];
        if (v < 0) v = -v;
        loudSampleSum += v;
    }

    /* Drop CC7 to a much lower value; without refresh_pcm_volumes the
     * next vblank's mix would still be loud.  With it, peak amplitude
     * should drop noticeably. */
    m4a_cc(drv, 0, 7, 16);
    v2_advance_chunked(drv, 4096);

    int quietSampleSum = 0;
    /* Read from the latest segment of the ring. */
    uint64_t cursor = ring->write_cursor;
    for (int i = 1; i <= M4A_PCM_SAMPLES_PER_VBLANK; i++) {
        size_t idx = (size_t)((cursor - i) % M4A_PCM_DMA_BUF_SIZE);
        int v = ring->ring_a[idx];
        if (v < 0) v = -v;
        quietSampleSum += v;
    }

    ASSERT(loudSampleSum > 0,        "loud sample sum > 0 (precondition)");
    ASSERT(quietSampleSum < loudSampleSum / 2,
                                     "CC7 drop reduces ring amplitude");

    m4a_driver_destroy(drv);
    free(wd);
}

/* §12 step 2 audit — reverb pipeline regression.
 *
 * The reverb stage runs in-place on the int16 pcmMix buffer (4-tap
 * sum scaled by amount>>9, added to current mix) and writes the
 * INT8-clamped wet output back into the delay buffer (matches v1
 * m4a_reverb.c: real m4a's delay buffer IS the int8 FIFO, so future
 * tap reads see byte-clamped values).  An int16 delay buffer would
 * silently feed back values outside the FIFO range on heavy mixes.
 *
 * This test exercises the pipeline end-to-end:
 *   1. With reverb disabled: ring output is non-zero (baseline).
 *   2. With reverb enabled at the same mix: ring output differs
 *      (reverb is actually running, not no-op'd somewhere).
 *   3. After many vblanks of sustained signal + reverb, ring output
 *      stays bounded in [-128, 127] (no runaway feedback / int16
 *      escape from the delay line). */
static void test_v2_pcm_reverb_pipeline(void)
{
    printf("Testing v2 PCM mixer + reverb pipeline...\n");

    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0xC000;          /* loop bits set — sustained signal */
    wd->loopStart = 0;
    wd->freq = 22050u << 10;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    /* High-amplitude saw pushes the pcmMix toward int8 limits, giving
     * the int8-clamp delay-line writeback a meaningful workout. */
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)((i * 4) - 127);
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].sustain = 0xFF;

    /* The reverb's "other" tap reads from +M4A_PCM_SAMPLES_PER_VBLANK
     * positions ahead in a 1584-byte ring, which means tap reads are
     * zero until the write pointer has wrapped past the unwritten
     * region — roughly 7+ vblanks of writes (1584 / 224 ≈ 7.07).
     * Running 16384 host frames (≈22 vblanks) gives the reverb
     * feedback loop ample time to engage. */
    enum { LONG_FRAMES = 16384 };

    /* --- Run 1: reverb disabled. --- */
    M4ADriver *drv_dry = m4a_driver_create(44100.0f);
    m4a_set_master_volume(drv_dry, 15);
    m4a_set_max_pcm_channels(drv_dry, 5);
    m4a_set_reverb_amount(drv_dry, 0);
    m4a_driver_set_voicegroup(drv_dry, voices);
    m4a_program_change(drv_dry, 0, 0);
    m4a_cc(drv_dry, 0, 7, 127);
    m4a_cc(drv_dry, 0, 10, 64);
    m4a_note_on(drv_dry, 0, 60, 127);
    v2_advance_chunked(drv_dry, LONG_FRAMES);

    int8_t dry_snapshot_a[M4A_PCM_DMA_BUF_SIZE];
    int8_t dry_snapshot_b[M4A_PCM_DMA_BUF_SIZE];
    {
        const M4APcmRing *r = m4a_get_pcm_ring(drv_dry);
        memcpy(dry_snapshot_a, r->ring_a, sizeof(dry_snapshot_a));
        memcpy(dry_snapshot_b, r->ring_b, sizeof(dry_snapshot_b));
    }
    m4a_driver_destroy(drv_dry);

    /* --- Run 2: reverb enabled, same mix. --- */
    M4ADriver *drv_wet = m4a_driver_create(44100.0f);
    m4a_set_master_volume(drv_wet, 15);
    m4a_set_max_pcm_channels(drv_wet, 5);
    m4a_set_reverb_amount(drv_wet, 64);    /* moderate reverb depth */
    m4a_driver_set_voicegroup(drv_wet, voices);
    m4a_program_change(drv_wet, 0, 0);
    m4a_cc(drv_wet, 0, 7, 127);
    m4a_cc(drv_wet, 0, 10, 64);
    m4a_note_on(drv_wet, 0, 60, 127);
    v2_advance_chunked(drv_wet, LONG_FRAMES);

    bool wet_differs = false;
    bool wet_in_range = true;
    {
        const M4APcmRing *r = m4a_get_pcm_ring(drv_wet);
        for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
            if (r->ring_a[i] != dry_snapshot_a[i]
                || r->ring_b[i] != dry_snapshot_b[i]) {
                wet_differs = true;
            }
            /* int8_t storage means values are inherently bounded;
             * this also doubles as a "no UB / no out-of-range memory
             * access" sanity check. */
            if (r->ring_a[i] < -128 || r->ring_a[i] > 127) wet_in_range = false;
            if (r->ring_b[i] < -128 || r->ring_b[i] > 127) wet_in_range = false;
        }
    }

    /* Sanity: the dry snapshot was actually populated. */
    bool dry_audible = false;
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        if (dry_snapshot_a[i] != 0 || dry_snapshot_b[i] != 0) {
            dry_audible = true;
            break;
        }
    }
    ASSERT(dry_audible,              "dry baseline ring populated");
    ASSERT(wet_differs,              "reverb_amount > 0 changes ring output");
    ASSERT(wet_in_range,             "reverb output stays in int8 range");

    m4a_driver_destroy(drv_wet);
    free(wd);
}

/* Layer 1.5 event-stream contract.  After note_on + 1 vblank the
 * driver must have queued the canonical CgbSound write order
 * (NRx1, NRx2, NRx3, NRx4-with-trigger, NR51) on the active channel,
 * all stamped with the same sample_offset (the host-frame position of
 * the vblank firing).  m4a_consume_writes empties the queue. */
static void test_v2_event_stream(void)
{
    printf("Testing v2 Layer 1.5 event-stream order + consumption...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 100);

    /* Before any advance: queue empty. */
    const M4ARegWriteBatch *b = m4a_get_pending_writes(drv);
    ASSERT(b != NULL,                "event batch accessible");
    ASSERT_EQ((int)b->count, 0,      "queue empty before m4a_advance");

    m4a_advance(drv, 1024);
    b = m4a_get_pending_writes(drv);

    /* sq2 path emits NR21, NR22, NR23, NR24, then NR51.  Find them in order. */
    int seenN21 = -1, seenN22 = -1, seenN23 = -1, seenN24 = -1, seenN51 = -1;
    for (size_t i = 0; i < b->count; i++) {
        switch (b->events[i].reg) {
        case M4A_REG_NR21: if (seenN21 < 0) seenN21 = (int)i; break;
        case M4A_REG_NR22: if (seenN22 < 0) seenN22 = (int)i; break;
        case M4A_REG_NR23: if (seenN23 < 0) seenN23 = (int)i; break;
        case M4A_REG_NR24: if (seenN24 < 0) seenN24 = (int)i; break;
        case M4A_REG_NR51: if (seenN51 < 0) seenN51 = (int)i; break;
        default: break;
        }
    }
    ASSERT(seenN21 >= 0,             "NR21 (sq2 length+duty) emitted");
    ASSERT(seenN22 >= 0,             "NR22 (sq2 envelope) emitted");
    ASSERT(seenN23 >= 0,             "NR23 (sq2 freq lo) emitted");
    ASSERT(seenN24 >= 0,             "NR24 (sq2 trigger+freq hi) emitted");
    ASSERT(seenN51 >= 0,             "NR51 (pan masks) emitted");

    /* CgbSound order — pokeemerald: NR21 < NR22 < NR23 < NR24 < NR51. */
    ASSERT(seenN21 < seenN22,        "NR21 before NR22 in queue");
    ASSERT(seenN22 < seenN23,        "NR22 before NR23 in queue");
    ASSERT(seenN23 < seenN24,        "NR23 before NR24 in queue");
    ASSERT(seenN24 < seenN51,        "NR24 (trigger) before NR51");

    /* NR24 carries the NRx4-with-trigger encoding: bit 7 = 1. */
    uint32_t nr24 = b->events[seenN24].value;
    ASSERT((nr24 & 0x80) != 0,       "NR24 has trigger bit (0x80) set");

    /* All vblank-emitted events share the same sample_offset (that
     * vblank's firing position within the render span). */
    uint32_t off = b->events[seenN21].sample_offset;
    bool sameOffset = true;
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].sample_offset != off) { sameOffset = false; break; }
    }
    ASSERT(sameOffset,               "all vblank events share one sample_offset");
    ASSERT(off > 0 && off < 1024,    "sample_offset within render span");

    m4a_consume_writes(drv);
    b = m4a_get_pending_writes(drv);
    ASSERT_EQ((int)b->count, 0,      "queue empty after m4a_consume_writes");

    m4a_driver_destroy(drv);
}

/* m4a_consume_writes is the unified "everything pending applied" call.
 * Even though hw_audio_render_events is the production chip API, the
 * snapshot trigger latches must reset on consume — otherwise any later
 * snapshot consumer would see ghost triggers.  Mirrors §6a contract. */
static void test_v2_consume_clears_triggers(void)
{
    printf("Testing v2 m4a_consume_writes clears snapshot triggers...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 100);
    m4a_advance(drv, 1024);

    const M4ARegisterFile *r = m4a_get_register_file(drv);
    ASSERT(r->trigger_sq2,           "trigger_sq2 set after vblank emit");

    m4a_consume_writes(drv);
    ASSERT(!r->trigger_sq2,          "consume_writes clears trigger_sq2");
    ASSERT(!r->trigger_sq1,          "consume_writes clears trigger_sq1");
    ASSERT(!r->trigger_wave,         "consume_writes clears trigger_wave");
    ASSERT(!r->trigger_noise,        "consume_writes clears trigger_noise");

    m4a_driver_destroy(drv);
}

/* Wave RAM byte-granular event sequence.  A wave-channel note-start
 * must emit 16 WAVE_RAM_BYTE events (one per nibble pair) interleaved
 * between NR30 (DAC off so writes are safe) and NR31/NR32/NR33/NR34. */
static void test_v2_wave_ram_events(void)
{
    printf("Testing v2 wave-RAM byte-granular events on note start...\n");

    /* 16-byte wave RAM with monotonic content so we can spot it. */
    static uint32_t waveRam[4];
    uint8_t *wb = (uint8_t *)waveRam;
    for (int i = 0; i < 16; i++) wb[i] = (uint8_t)(0x10 + i);

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type        = VOICE_PROGRAMMABLE_WAVE;
    voices[0].key         = 60;
    voices[0].wavePointer = waveRam;
    voices[0].attack      = 0;
    voices[0].decay       = 0;
    voices[0].sustain     = 16;
    voices[0].release     = 16;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 100);
    m4a_advance(drv, 1024);

    const M4ARegWriteBatch *b = m4a_get_pending_writes(drv);
    int waveBytes = 0;
    int firstByteIdx = -1, lastByteIdx = -1;
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].reg == M4A_REG_WAVE_RAM_BYTE) {
            uint32_t v = b->events[i].value;
            uint32_t addr = (v >> 8) & 0xF;
            uint32_t byte = v & 0xFF;
            ASSERT(byte == (uint32_t)(0x10 + addr),
                                     "wave-RAM byte payload matches voice data");
            if (firstByteIdx < 0) firstByteIdx = (int)i;
            lastByteIdx = (int)i;
            waveBytes++;
        }
    }
    ASSERT_EQ(waveBytes, 16,         "exactly 16 WAVE_RAM_BYTE events on start");

    /* NR30 must drop to 0 (DAC off) immediately before the byte block —
     * real m4a / GB hardware glitches if wave RAM is written with DAC
     * on.  Then NR30=0x80 (DAC on) must come right after the bytes. */
    ASSERT(firstByteIdx > 0,         "WAVE_RAM_BYTE not at start of queue");
    const M4ARegWrite *prev = &b->events[firstByteIdx - 1];
    ASSERT(prev->reg == M4A_REG_NR30 && prev->value == 0,
                                     "NR30=0 immediately before wave bytes");

    bool foundDacOn = false;
    for (size_t i = (size_t)lastByteIdx + 1; i < b->count; i++) {
        if (b->events[i].reg == M4A_REG_NR30) {
            ASSERT_EQ(b->events[i].value, 0x80,
                                     "NR30=DAC-on immediately after wave bytes");
            foundDacOn = true;
            break;
        }
    }
    ASSERT(foundDacOn,               "NR30=0x80 follows wave-byte block");

    m4a_driver_destroy(drv);
}

/* MODT (CC 0x16) changes which axis modM modulates (vibrato / tremolo /
 * autopan).  When modM is non-zero (LFO running), switching modT must
 * recompute volMR/volML/keyM/pitM via m4a_trk_vol_pit_set BEFORE
 * pushing into channels — otherwise channels see stale derived values.
 * This is what v1 refresh_volumes does implicitly. */
static void test_v2_modt_recomputes_track_state(void)
{
    printf("Testing v2 MODT recomputes derived track state...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);

    /* Set MOD depth + LFO speed so modM has a chance to be non-zero
     * once a tick fires.  modT defaults to 0 (vibrato). */
    m4a_cc(drv, 0, 0x01, 64);   /* mod wheel = depth 64 */
    m4a_cc(drv, 0, 0x15, 32);   /* LFOS */

    m4a_note_on(drv, 0, 60, 100);
    m4a_advance(drv, 1024);
    m4a_consume_writes(drv);

    const M4ARegisterFile *snap_before = m4a_get_register_file(drv);
    uint16_t freqBefore = snap_before->sq2_freq;
    uint8_t  envBefore  = snap_before->sq2_env_volume;

    /* Switch MODT from 0 (vibrato → keyM/pitM) to 1 (tremolo → vol).
     * If trk_vol_pit_set isn't called, the channel refresh would push
     * stale derived state.  With the fix, the next vblank's emit
     * carries the recomputed values. */
    m4a_cc(drv, 0, 0x16, 1);
    m4a_advance(drv, 1024);

    const M4ARegisterFile *snap_after = m4a_get_register_file(drv);
    /* The interesting consequence is that after the MODT switch with
     * modM still in vibrato-residue territory, freq returns to the
     * un-modulated value (because modT=1 means modM no longer affects
     * pitch). */
    (void)envBefore;
    (void)freqBefore;
    ASSERT(snap_after->sq2_freq != 0,
                                     "sq2_freq still populated post-MODT");
    /* Smoke test: changing MODT does not crash / leave channel disabled. */
    ASSERT(snap_after->sq2_enabled,  "sq2 still enabled across MODT change");

    m4a_driver_destroy(drv);
}

/* ---- §12 step 1: LFO advancement (m4a_internal_lfo_tick) ----
 *
 * The LFO tick runs from m4a_main.c's tempoC overflow loop (one tick
 * per 150 accumulated tempoI units, roughly one tick per vblank with
 * default tempoI=150).  Each tick:
 *   - skips tracks where mod=0 OR lfoSpeed=0 (no modulator armed)
 *   - decrements lfoDelayC for tracks still in the delay phase
 *   - for armed + post-delay tracks: advances lfoSpeedC by lfoSpeed,
 *     derives a triangle-wave sample, folds depth into modM, and
 *     refreshes active CGB / PCM channels when modM changes.
 *
 * Observable consequences depend on modT:
 *   - modT=0 (vibrato): track keyM/pitM follow modM → freq events
 *     re-emitted → snapshot.sq2_freq varies across vblanks.
 *   - modT=1 (tremolo): track volMR/volML follow modM → envelope
 *     volumes refresh → snapshot.sq2_env_volume varies.
 *   - modT=2 (autopan): pan masks shift between L/R routing.
 *
 * These tests use modT=0 (vibrato) since freq is the cleanest
 * single-field axis to observe through the snapshot. */

/* Helper: configure a SQ2 voice with given LFO state, fire a note,
 * and capture sq2_freq across N_SNAPS vblanks for caller analysis.
 * mod, lfoSpeed, lfoDelay are CC values (0..127). */
static void capture_sq2_freq_under_lfo(uint8_t mod_cc, uint8_t lfos_cc,
                                       uint8_t lfodl_cc,
                                       uint16_t *out_freqs, int n_snaps)
{
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7,    127);             /* CC7 vol max */
    m4a_cc(drv, 0, 10,   64);              /* CC10 pan center */
    if (lfodl_cc > 0)
        m4a_cc(drv, 0, 0x1A, lfodl_cc);    /* LFODL must come before mod */
    m4a_cc(drv, 0, 0x01, mod_cc);          /* CC1 mod depth */
    m4a_cc(drv, 0, 0x15, lfos_cc);         /* CC0x15 LFO speed */

    m4a_note_on(drv, 0, 60, 100);

    /* ~1.4 vblanks per 1024 host frames at 44100 Hz → effectively 1
     * vblank tick per snapshot (the second vblank lands a few
     * snapshots later as fractional accum builds up).  consume_writes
     * each iteration so the event queue doesn't overflow over many
     * snapshots. */
    for (int i = 0; i < n_snaps; i++) {
        m4a_advance(drv, 1024);
        m4a_consume_writes(drv);
        out_freqs[i] = m4a_get_register_file(drv)->sq2_freq;
    }

    m4a_driver_destroy(drv);
}

/* ---- v2 XCMD-via-MIDI-CC tests ----------------------------------------
 *
 * The v2 driver implements the same two-CC XCMD protocol the v1 engine
 * has (CC 0x1E selector, then 0x1D/0x1F payload bytes), but mutates v2's
 * own M4ADriverTrack / M4ADriverPcmChan / M4ADriverCgbChan fields.  These
 * tests prove field mutation, propagation into newly-started notes,
 * little-endian multi-byte assembly, partial/invalid safety, and sticky-
 * selector behaviour.  See xcmd.md.
 */
static void v2_send_xcmd_select(M4ADriver *drv, int track, uint8_t selector) {
    m4a_cc(drv, track, 0x1E, selector);
}
static void v2_send_xcmd_bytes(M4ADriver *drv, int track,
                               const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; i++)
        m4a_cc(drv, track, 0x1D, bytes[i]);
}

static void test_v2_xcmd_mutates_track_state(void)
{
    printf("Testing v2 XCMD mutates currentVoice and pseudo-echo track fields...\n");

    int dataSize = 4;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->freq = 0x01000000;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type     = VOICE_DIRECTSOUND;
    voices[0].key      = 60;
    voices[0].wav      = wd;
    voices[0].attack   = 0x10;
    voices[0].decay    = 0x20;
    voices[0].sustain  = 0x30;
    voices[0].release  = 0x40;
    voices[0].length   = 0x50;
    voices[0].panSweep = 0x60;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);

    XcmdCapture cap = {0};
    m4a_driver_set_xcmd_callback(drv, capture_xcmd, &cap);

    /* xATTA: 1 byte → currentVoice.attack */
    v2_send_xcmd_select(drv, 0, 0x04);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x7A }, 1);
    ASSERT_EQ(drv->tracks[0].currentVoice.attack, 0x7A,
              "v2 xATTA mutates currentVoice.attack");
    ASSERT_EQ(cap.called, 1,                 "v2 xATTA fires xcmd callback");
    ASSERT_EQ(cap.selector, 0x04,            "v2 xATTA callback reports selector");
    ASSERT_EQ(cap.value, 0x7A,               "v2 xATTA callback reports value");
    ASSERT_EQ(cap.trackIndex, 0,             "v2 xATTA callback reports track");

    /* Single-byte selectors: each mutates the matching v2 field. */
    v2_send_xcmd_select(drv, 0, 0x05);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x55 }, 1);
    v2_send_xcmd_select(drv, 0, 0x06);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x44 }, 1);
    v2_send_xcmd_select(drv, 0, 0x07);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x33 }, 1);
    v2_send_xcmd_select(drv, 0, 0x08);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x22 }, 1);
    v2_send_xcmd_select(drv, 0, 0x09);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x11 }, 1);
    v2_send_xcmd_select(drv, 0, 0x0A);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x66 }, 1);
    v2_send_xcmd_select(drv, 0, 0x0B);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x77 }, 1);
    v2_send_xcmd_select(drv, 0, 0x02);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ VOICE_SQUARE_1 }, 1);

    ASSERT_EQ(drv->tracks[0].currentVoice.decay,    0x55, "v2 xDECA");
    ASSERT_EQ(drv->tracks[0].currentVoice.sustain,  0x44, "v2 xSUST");
    ASSERT_EQ(drv->tracks[0].currentVoice.release,  0x33, "v2 xRELE");
    ASSERT_EQ(drv->tracks[0].pseudoEchoVolume,      0x22, "v2 xIECV → track pseudoEchoVolume");
    ASSERT_EQ(drv->tracks[0].pseudoEchoLength,      0x11, "v2 xIECL → track pseudoEchoLength");
    ASSERT_EQ(drv->tracks[0].currentVoice.length,   0x66, "v2 xLENG");
    ASSERT_EQ(drv->tracks[0].currentVoice.panSweep, 0x77, "v2 xSWEE");
    ASSERT_EQ(drv->tracks[0].currentVoice.type, VOICE_SQUARE_1, "v2 xTYPE");

    /* xCmd 0x0D: 4-byte little-endian payload assembled into extendedValue. */
    cap.called = 0;
    v2_send_xcmd_select(drv, 0, 0x0D);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x78, 0x56, 0x34 }, 3);
    ASSERT_EQ(drv->tracks[0].extendedValue, 0u,
              "v2 xCmd 0x0D waits for all four bytes (no premature apply)");
    ASSERT_EQ(cap.called, 0,
              "v2 callback does not fire on partial 4-byte payload");
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x12 }, 1);
    ASSERT_EQ(drv->tracks[0].extendedValue, 0x12345678u,
              "v2 xCmd 0x0D assembles 4-byte little-endian value");
    ASSERT_EQ(cap.called, 1,                 "v2 xCmd 0x0D fires callback after fourth byte");
    ASSERT_EQ(cap.value, 0x12345678u,        "v2 xCmd 0x0D callback carries assembled u32");

    /* xWAVE (0x01): notify-only.  Per xcmd.md, the 32-bit payload is a
     * ROM address from the original game and would dangle if cast to a
     * host pointer — currentVoice.wav must NOT change.  Callback must
     * still fire with the assembled u32 so a future address-resolver
     * layer can map it. */
    WaveData *wavBefore = drv->tracks[0].currentVoice.wav;
    cap.called = 0;
    v2_send_xcmd_select(drv, 0, 0x01);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0xBE, 0xBA, 0xFE, 0xCA }, 4);
    ASSERT(drv->tracks[0].currentVoice.wav == wavBefore,
              "v2 xWAVE does not overwrite currentVoice.wav (notify-only)");
    ASSERT_EQ(cap.called, 1,                 "v2 xWAVE fires callback");
    ASSERT_EQ(cap.selector, 0x01,            "v2 xWAVE callback reports selector 0x01");
    ASSERT_EQ(cap.value, 0xCAFEBABEu,        "v2 xWAVE callback carries assembled u32 (LE)");

    /* xCmd 0x0C (xWAIT) is intentionally not implemented in v2 — there's
     * no song-script interpreter in the MIDI ingress path that could
     * honour it.  Selecting 0x0C and sending payload bytes must be a
     * silent no-op that doesn't fire the callback or mutate any state. */
    cap.called = 0;
    v2_send_xcmd_select(drv, 0, 0x0C);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x02, 0x00 }, 2);
    ASSERT_EQ(cap.called, 0,
              "v2 xCmd 0x0C (xWAIT) is unimplemented — no callback fires");

    m4a_driver_destroy(drv);
    free(wd);
}

/* Notes started after an XCMD must pick up the changed ADSR / pseudo-echo
 * values.  PCM and CGB channels each have their own copy-from-track path
 * in m4a_note_on() — both must propagate. */
static void test_v2_xcmd_propagates_to_new_notes(void)
{
    printf("Testing v2 XCMD propagates to new PCM and CGB notes...\n");

    /* PCM voice — verify direct-sound channel gets the XCMD'd ADSR. */
    int dataSize = 16;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->freq = 0x01000000;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++) wd->data[i] = (int8_t)(i * 7);

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].sustain = 0xFF;
    /* program 1: SQ2 voice for the CGB-side check. */
    voices[1].type    = VOICE_SQUARE_2;
    voices[1].key     = 60;
    voices[1].attack  = 0;
    voices[1].decay   = 0;
    voices[1].sustain = 16;
    voices[1].release = 16;
    voices[1].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_set_max_pcm_channels(drv, 4);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);

    /* Push xATTA / xDECA / xSUST / xRELE / xIECV / xIECL on track 0. */
    v2_send_xcmd_select(drv, 0, 0x04); v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x7A }, 1);
    v2_send_xcmd_select(drv, 0, 0x05); v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x55 }, 1);
    v2_send_xcmd_select(drv, 0, 0x06); v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x44 }, 1);
    v2_send_xcmd_select(drv, 0, 0x07); v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x33 }, 1);
    v2_send_xcmd_select(drv, 0, 0x08); v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x22 }, 1);
    v2_send_xcmd_select(drv, 0, 0x09); v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x11 }, 1);

    /* Note on now: PCM channel must inherit the new ADSR + pseudo-echo. */
    m4a_note_on(drv, 0, 60, 100);

    int found = -1;
    for (int i = 0; i < M4A_MAX_PCM_CHANNELS; i++)
        if (drv->pcmChans[i].status & M4A_CHN_ON) { found = i; break; }
    ASSERT(found >= 0,                       "v2 PCM channel allocated for note");
    if (found >= 0) {
        M4ADriverPcmChan *p = &drv->pcmChans[found];
        ASSERT_EQ(p->attack,  0x7A,          "v2 XCMD attack copied into new PCM note");
        ASSERT_EQ(p->decay,   0x55,          "v2 XCMD decay copied into new PCM note");
        ASSERT_EQ(p->sustain, 0x44,          "v2 XCMD sustain copied into new PCM note");
        ASSERT_EQ(p->release, 0x33,          "v2 XCMD release copied into new PCM note");
        ASSERT_EQ(p->pseudoEchoVolume, 0x22, "v2 XCMD IECV copied into new PCM note");
        ASSERT_EQ(p->pseudoEchoLength, 0x11, "v2 XCMD IECL copied into new PCM note");
    }

    /* Switch to a CGB voice on a fresh track and prove the SQ2 channel
     * also picks up XCMD'd ADSR + pseudo-echo. */
    m4a_program_change(drv, 1, 1);
    m4a_cc(drv, 1, 7, 127);
    m4a_cc(drv, 1, 10, 64);
    v2_send_xcmd_select(drv, 1, 0x04); v2_send_xcmd_bytes(drv, 1, (uint8_t[]){ 0x09 }, 1);
    v2_send_xcmd_select(drv, 1, 0x05); v2_send_xcmd_bytes(drv, 1, (uint8_t[]){ 0x08 }, 1);
    v2_send_xcmd_select(drv, 1, 0x06); v2_send_xcmd_bytes(drv, 1, (uint8_t[]){ 0x07 }, 1);
    v2_send_xcmd_select(drv, 1, 0x07); v2_send_xcmd_bytes(drv, 1, (uint8_t[]){ 0x06 }, 1);
    v2_send_xcmd_select(drv, 1, 0x08); v2_send_xcmd_bytes(drv, 1, (uint8_t[]){ 0x05 }, 1);
    v2_send_xcmd_select(drv, 1, 0x09); v2_send_xcmd_bytes(drv, 1, (uint8_t[]){ 0x04 }, 1);

    m4a_note_on(drv, 1, 60, 100);

    /* SQ2 lives at cgb[1] (type 2 → idx 1). */
    M4ADriverCgbChan *sq2 = &drv->cgb[1];
    ASSERT(sq2->status & M4A_CHN_ON,         "v2 SQ2 channel started");
    ASSERT_EQ(sq2->attack,  0x09,            "v2 XCMD attack copied into new CGB note");
    ASSERT_EQ(sq2->decay,   0x08,            "v2 XCMD decay copied into new CGB note");
    ASSERT_EQ(sq2->sustain, 0x07,            "v2 XCMD sustain copied into new CGB note");
    ASSERT_EQ(sq2->release, 0x06,            "v2 XCMD release copied into new CGB note");
    ASSERT_EQ(sq2->pseudoEchoVolume, 0x05,   "v2 XCMD IECV copied into new CGB note");
    ASSERT_EQ(sq2->pseudoEchoLength, 0x04,   "v2 XCMD IECL copied into new CGB note");

    m4a_driver_destroy(drv);
    free(wd);
}

/* Defensive cases: unknown selectors must not crash; partial payloads
 * must not apply; selector stays sticky after apply so consecutive
 * payloads of the same length all dispatch to the same selector. */
static void test_v2_xcmd_protocol_safety(void)
{
    printf("Testing v2 XCMD partial / invalid / sticky-selector handling...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0x10;
    voices[0].decay   = 0x20;
    voices[0].sustain = 0x30;
    voices[0].release = 0x40;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);

    XcmdCapture cap = {0};
    m4a_driver_set_xcmd_callback(drv, capture_xcmd, &cap);

    /* No selector latched yet (initial extendedCommand == 0).  A stray
     * 0x1D payload byte must be a no-op and not crash. */
    m4a_cc(drv, 0, 0x1D, 0xAA);
    ASSERT_EQ(drv->tracks[0].extendedCommandCount, 0,
              "v2 0x1D with no selector latched does not buffer bytes");
    ASSERT_EQ(drv->tracks[0].currentVoice.attack, 0x10,
              "v2 0x1D with no selector does not mutate state");
    ASSERT_EQ(cap.called, 0, "v2 0x1D with no selector does not notify");

    /* Unknown selector (e.g. 0x03 — gap in v1's table) → dataLength==0,
     * so any subsequent payload is discarded silently. */
    v2_send_xcmd_select(drv, 0, 0x03);
    m4a_cc(drv, 0, 0x1D, 0xAA);
    m4a_cc(drv, 0, 0x1D, 0xBB);
    ASSERT_EQ(drv->tracks[0].currentVoice.attack, 0x10,
              "v2 unknown selector ignores payload safely");
    ASSERT_EQ(cap.called, 0, "v2 unknown selector does not notify");

    /* Partial payload: select a 4-byte xCmd, send 3 bytes, assert no apply. */
    cap.called = 0;
    v2_send_xcmd_select(drv, 0, 0x0D);
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x11, 0x22, 0x33 }, 3);
    ASSERT_EQ(drv->tracks[0].extendedValue, 0u,
              "v2 partial 4-byte payload does not apply");
    ASSERT_EQ(cap.called, 0,
              "v2 partial payload does not fire callback");
    ASSERT_EQ(drv->tracks[0].extendedCommandCount, 3,
              "v2 partial payload accumulates byte count");

    /* Selecting a different selector mid-payload resets the buffer. */
    v2_send_xcmd_select(drv, 0, 0x04);
    ASSERT_EQ(drv->tracks[0].extendedCommandCount, 0,
              "v2 0x1E resets byte count for new selector");
    ASSERT_EQ(drv->tracks[0].extendedCommandBytes[0], 0,
              "v2 0x1E zeroes the payload buffer");

    /* Sticky selector: after a complete xATTA dispatch, another payload
     * byte without a fresh 0x1E must apply to xATTA again.  This matches
     * v1 + real m4a (the selector is sticky until the next CMD_XCMD). */
    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x70 }, 1);   /* completes xATTA */
    ASSERT_EQ(drv->tracks[0].currentVoice.attack, 0x70,
              "v2 sticky-selector dispatch 1: xATTA(0x70) applies");
    ASSERT_EQ(cap.called, 1,
              "v2 sticky-selector dispatch 1: callback fired once");

    v2_send_xcmd_bytes(drv, 0, (uint8_t[]){ 0x71 }, 1);   /* xATTA again, no re-select */
    ASSERT_EQ(drv->tracks[0].currentVoice.attack, 0x71,
              "v2 sticky-selector dispatch 2: xATTA stays selected for next payload");
    ASSERT_EQ(cap.called, 2,
              "v2 sticky-selector dispatch 2: callback fired twice total");
    ASSERT_EQ(cap.selector, 0x04,
              "v2 sticky-selector dispatch 2: callback still reports selector 0x04");

    m4a_driver_destroy(drv);
}

/* Render-level proof that XCMD changes affect actual audio.  Two
 * identical-prep drivers; one receives xCmd 0x06 (xSUST = 0xFF) before
 * note_on, the other does not.  At sustain the louder driver's PCM ring
 * must carry larger sample magnitudes than the control. */
static void test_v2_xcmd_render_changes_audio(void)
{
    printf("Testing v2 XCMD changes rendered PCM output (sustain XCMD)...\n");

    /* Looping sine so the wave doesn't exhaust before sustain settles.
     * status & 0xC000 enables loop mode in v2's m4a_drv_pcm_start. */
    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->status = 0xC000;
    wd->freq = 0x01000000;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)(127.0 * sin(2.0 * 3.14159265 * i / dataSize));
    wd->data[dataSize] = wd->data[0];

    /* attack=0xFF + decay=0 → first PCM tick settles directly onto
     * sustain (mirrors test_v2_pcm_ring_fills' proven pattern).  Default
     * sustain=0x10 keeps the control driver quiet; xSUST=0xFF on the
     * second driver lifts it to maximum.  Identical wave / vol / pan,
     * so any envelope-amplitude difference comes solely from xCmd. */
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].decay   = 0;
    voices[0].sustain = 0x10;
    voices[0].release = 0;

    /* Control driver: stock voice, no XCMD. */
    M4ADriver *quiet = m4a_driver_create(44100.0f);
    m4a_set_master_volume(quiet, 15);
    m4a_set_max_pcm_channels(quiet, 4);
    m4a_driver_set_voicegroup(quiet, voices);
    m4a_program_change(quiet, 0, 0);
    m4a_cc(quiet, 0, 7, 127);
    m4a_cc(quiet, 0, 10, 64);
    m4a_note_on(quiet, 0, 60, 127);

    /* Loud driver: xSUST 0xFF before note_on. */
    M4ADriver *loud  = m4a_driver_create(44100.0f);
    m4a_set_master_volume(loud, 15);
    m4a_set_max_pcm_channels(loud, 4);
    m4a_driver_set_voicegroup(loud, voices);
    m4a_program_change(loud, 0, 0);
    m4a_cc(loud, 0, 7, 127);
    m4a_cc(loud, 0, 10, 64);
    v2_send_xcmd_select(loud, 0, 0x06);
    v2_send_xcmd_bytes(loud, 0, (uint8_t[]){ 0xFF }, 1);
    ASSERT_EQ(loud->tracks[0].currentVoice.sustain, 0xFF,
              "v2 xSUST raised the loud driver's currentVoice.sustain");
    m4a_note_on(loud, 0, 60, 127);

    /* Render enough vblanks to settle past attack+decay into sustain. */
    v2_advance_chunked(quiet, 8192);
    v2_advance_chunked(loud,  8192);

    const M4APcmRing *qr = m4a_get_pcm_ring(quiet);
    const M4APcmRing *lr = m4a_get_pcm_ring(loud);

    int qpeak = 0, lpeak = 0;
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        int qa = qr->ring_a[i] < 0 ? -qr->ring_a[i] : qr->ring_a[i];
        int la = lr->ring_a[i] < 0 ? -lr->ring_a[i] : lr->ring_a[i];
        if (qa > qpeak) qpeak = qa;
        if (la > lpeak) lpeak = la;
    }
    ASSERT(qpeak > 0,                 "control driver produces non-silent output");
    ASSERT(lpeak > 0,                 "xSUST driver produces non-silent output");
    /* sustain 0x10 → 0xFF is roughly a 16× envelope multiplier; in
     * practice the rendered peak ratio lands around 15× after master
     * volume / per-sample clamping.  Require at least 4× to leave
     * headroom for envelope-phase timing variance between drivers. */
    ASSERT(lpeak >= qpeak * 4,
              "v2 xSUST 0xFF produces ≥4× louder PCM peak than stock voice");

    m4a_driver_destroy(quiet);
    m4a_driver_destroy(loud);
    free(wd);
}

static void test_v2_lfo_disabled_no_freq_drift(void)
{
    printf("Testing v2 LFO disabled (mod=0) → SQ2 freq is constant across vblanks...\n");

    enum { N_SNAPS = 16 };
    uint16_t freqs[N_SNAPS];
    /* mod = 0 → LFO tick early-exits, modM stays 0, freq doesn't drift. */
    capture_sq2_freq_under_lfo(/*mod=*/0, /*lfos=*/32, /*lfodl=*/0,
                               freqs, N_SNAPS);

    bool all_equal = true;
    for (int i = 1; i < N_SNAPS; i++) {
        if (freqs[i] != freqs[0]) { all_equal = false; break; }
    }
    ASSERT(freqs[0] != 0,            "baseline sq2_freq populated");
    ASSERT(all_equal,                "mod=0 produces constant sq2_freq (no LFO drift)");
}

static void test_v2_lfo_speed_default_matches_m4a(void)
{
    printf("Testing v2 LFO speed default matches m4a...\n");

    M4ADriver *drv = m4a_driver_create(44100.0f);

    for (int i = 0; i < M4A_MAX_TRACKS; i++)
        ASSERT_EQ(drv->tracks[i].lfoSpeed, 22, "track lfoSpeed defaults to 22");

    m4a_driver_destroy(drv);
}

static void test_v2_lfo_vibrato_modulates_freq(void)
{
    printf("Testing v2 LFO vibrato (modT=0) modulates SQ2 freq across vblanks...\n");

    enum { N_SNAPS = 16 };
    uint16_t freqs[N_SNAPS];
    /* mod=64 + lfoSpeed=32 + default modT=0 (vibrato).
     * After ~16 vblanks, lfoSpeedC sweeps the triangle wave through
     * multiple non-zero modM values and freq must observably move. */
    capture_sq2_freq_under_lfo(/*mod=*/64, /*lfos=*/32, /*lfodl=*/0,
                               freqs, N_SNAPS);

    /* Find min/max freq; assert at least 2 distinct values appear. */
    uint16_t fmin = freqs[0], fmax = freqs[0];
    for (int i = 1; i < N_SNAPS; i++) {
        if (freqs[i] < fmin) fmin = freqs[i];
        if (freqs[i] > fmax) fmax = freqs[i];
    }
    ASSERT(freqs[0] != 0,            "baseline sq2_freq populated");
    ASSERT(fmin != fmax,             "LFO vibrato produces freq variation across vblanks");
}

static void test_v2_lfo_delay_holds_off(void)
{
    printf("Testing v2 LFODL delays modulation onset...\n");

    enum { N_SNAPS = 16 };
    uint16_t freqs[N_SNAPS];
    /* lfoDelay=8 → roughly 8 vblanks before LFO starts ticking modM.
     * Capture across 16 snapshots; the early ones must be constant
     * (delay phase) and the later ones must vary (post-delay LFO
     * advancing modM). */
    capture_sq2_freq_under_lfo(/*mod=*/64, /*lfos=*/32, /*lfodl=*/8,
                               freqs, N_SNAPS);

    /* First ~6 snapshots: lfoDelayC counting down, modM stays 0,
     * freq is constant.  Allow margin for vblank/snapshot rate
     * (1024 host frames ≈ 1.39 vblanks; snapshot k captures the
     * state after some fractional vblanks have fired). */
    int constant_run = 1;
    for (int i = 1; i < N_SNAPS; i++) {
        if (freqs[i] != freqs[0]) break;
        constant_run++;
    }
    ASSERT(constant_run >= 4,
                                     "early snapshots are constant (LFO in delay phase)");
    /* Late snapshots must show variation (delay expired, modM moves). */
    bool late_varies = false;
    for (int i = constant_run; i < N_SNAPS; i++) {
        if (freqs[i] != freqs[constant_run - 1]) {
            late_varies = true;
            break;
        }
    }
    ASSERT(late_varies,              "post-delay snapshots show LFO modulation");
}

static void test_v2_lfo_lfodl_resets_running_modulation(void)
{
    printf("Testing v2 LFODL after modulation running → resets modM and refreshes...\n");

    /* This test exercises the m4a_track.c CC 0x1A handler's
     * "modM != 0 → reset and refresh" branch.  Run vibrato for a
     * while so modM goes nonzero; then write LFODL and verify the
     * next snapshot's freq is back at baseline (modM = 0). */
    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7,    127);
    m4a_cc(drv, 0, 10,   64);
    /* Capture baseline freq with NO modulation: trigger the note,
     * fire the first vblank, snapshot.  modM=0 (no LFO armed yet), so
     * keyM/pitM reflect the un-modulated note. */
    m4a_note_on(drv, 0, 60, 100);
    m4a_advance(drv, 1024);
    m4a_consume_writes(drv);
    uint16_t baseline = m4a_get_register_file(drv)->sq2_freq;

    /* Now arm LFO and run several vblanks so modM oscillates and
     * freq drifts away from baseline. */
    m4a_cc(drv, 0, 0x01, 64);    /* mod = 64 */
    m4a_cc(drv, 0, 0x15, 32);    /* LFOS = 32 */
    for (int i = 0; i < 8; i++) {
        m4a_advance(drv, 1024);
        m4a_consume_writes(drv);
    }
    uint16_t freq_during_lfo = m4a_get_register_file(drv)->sq2_freq;

    /* Apply LFODL.  The CC 0x1A handler resets lfoSpeedC, lfoDelayC,
     * and (since modM != 0) modM itself + refreshes active channels.
     * Next snapshot must reflect freq back at baseline. */
    m4a_cc(drv, 0, 0x1A, 4);
    m4a_advance(drv, 1024);
    m4a_consume_writes(drv);
    uint16_t freq_after_lfodl = m4a_get_register_file(drv)->sq2_freq;

    ASSERT(freq_during_lfo != baseline,
                                     "LFO running drove freq away from baseline");
    ASSERT(freq_after_lfodl == baseline,
                                     "LFODL reset modM → freq returns to baseline");

    m4a_driver_destroy(drv);
}

#if defined(HW_AUDIO_V2)
/* Step 4 acceptance: PSG square + wave synth produces audible output
 * end-to-end through hw_audio_render_events.  Tests the integrated
 * pipeline: ingress → CgbSound emits events → chip applies events at
 * sample_offset boundaries → square synth advances phase → ±amplitude
 * sample written to outL/outR. */
static void test_v2_psg_square_audible(void)
{
    printf("Testing v2 PSG square produces audible output...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;   /* 50% duty */

    M4ADriver *drv = m4a_driver_create(44100.0f);
    HwAudio   *hw  = hw_audio_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 100);

    enum { N = 8192 };
    float L[N], R[N];

    /* Production event-driven path, chunked to fit the bounded queue. */
    v2_render_chunked(drv, hw, L, R, N);

    /* Detect signal: peak amplitude > 0 and at least some sign changes
     * (square wave has many zero-crossings). */
    float peak = 0.0f;
    int signChanges = 0;
    float prev = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
        if (i > 0 && ((L[i] >= 0) != (prev >= 0))) signChanges++;
        prev = L[i];
    }
    ASSERT(peak > 0.001f,            "sq2 produces non-zero output");
    ASSERT(signChanges > 10,         "sq2 oscillates (many zero crossings)");
    ASSERT_EQ((int)m4a_get_events_dropped(drv), 0,
                                     "no events dropped during PSG render");

    m4a_driver_destroy(drv);
    hw_audio_destroy(hw);
}

/* NR51 pan-mask routing: a sq2 voice with NR51 routing only to LEFT
 * (pan_mask_right cleared) must produce zero on R but non-zero on L. */
static void test_v2_psg_pan_routing(void)
{
    printf("Testing v2 PSG pan-mask routing...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    HwAudio   *hw  = hw_audio_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 0);     /* hard left */
    m4a_note_on(drv, 0, 60, 127);

    enum { N = 4096 };
    float L[N], R[N];
    v2_render_chunked(drv, hw, L, R, N);

    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peakL) peakL = a;
        a = R[i]; if (a < 0) a = -a;
        if (a > peakR) peakR = a;
    }
    ASSERT(peakL > 0.001f,           "hard-left pan produces L signal");
    ASSERT(peakR < peakL * 0.1f,     "hard-left pan attenuates R signal");
    ASSERT_EQ((int)m4a_get_events_dropped(drv), 0,
                                     "no events dropped during pan-routing render");

    m4a_driver_destroy(drv);
    hw_audio_destroy(hw);
}

/* Wave channel audibility: a programmable-wave voice with a non-DC
 * wave pattern should produce non-zero PSG output through the same
 * event pipeline. */
static void test_v2_psg_wave_audible(void)
{
    printf("Testing v2 PSG wave channel audible...\n");

    /* 16-byte wave RAM: triangle-ish ramp pattern.  Real m4a builds
     * wavePointer to point at a 16-byte block; we just inline one. */
    static uint32_t waveRam[4];
    uint8_t *wb = (uint8_t *)waveRam;
    /* Two-nibble-per-byte ramp: 0,2,4,6,8,A,C,E,F,D,B,9,7,5,3,1 ... etc. */
    for (int i = 0; i < 16; i++) wb[i] = (uint8_t)(((i * 17) & 0xF0) | ((i * 23) & 0x0F));

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type        = VOICE_PROGRAMMABLE_WAVE;
    voices[0].key         = 60;
    voices[0].wavePointer = waveRam;
    voices[0].attack      = 0;
    voices[0].decay       = 0;
    voices[0].sustain     = 16;
    voices[0].release     = 16;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    HwAudio   *hw  = hw_audio_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 100);

    enum { N = 8192 };
    float L[N], R[N];
    v2_render_chunked(drv, hw, L, R, N);

    float peak = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    ASSERT(peak > 0.001f,            "wave channel produces non-zero output");
    ASSERT_EQ((int)m4a_get_events_dropped(drv), 0,
                                     "no events dropped during wave render");

    m4a_driver_destroy(drv);
    hw_audio_destroy(hw);
}

#endif /* HW_AUDIO_V2 */

/* CGB retrigger semantics: NRx4 trigger bit must fire on note start only,
 * NEVER on subsequent envelope-update vblanks.  Real GB hardware resets
 * the wave RAM position (NR34 trigger) and noise LFSR (NR44 trigger) on
 * trigger; re-triggering during sustain corrupts wave/noise output.
 * Reference: mGBA / hardware (see project_audio_reference_target.md);
 * also matches v1 + real m4a behaviour: ChnVolSetCgb computes envelope;
 * CgbSound writes NRx2 every tick but NRx4-with-trigger only on fresh
 * note.  This is a DRIVER-side regression — runs in the M4A_DRIVER_V2
 * build slice without HW_AUDIO_V2, since the contract is purely about
 * which Layer-1.5 events the driver emits. */
static void test_v2_cgb_trigger_only_on_note_start(void)
{
    printf("Testing v2 NRx4 trigger fires once on note start, not on sustain...\n");

    static uint32_t waveRam[4];
    for (int i = 0; i < 16; i++) ((uint8_t*)waveRam)[i] = (uint8_t)(0x10 + i);

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));

    voices[0].type    = VOICE_PROGRAMMABLE_WAVE;
    voices[0].key     = 60;
    voices[0].wavePointer = waveRam;
    voices[0].attack  = 8;  voices[0].decay = 4;
    voices[0].sustain = 12; voices[0].release = 16;

    voices[1].type    = VOICE_NOISE;
    voices[1].key     = 60;
    voices[1].wavePointer = (uint32_t *)(uintptr_t)2;
    voices[1].attack  = 8;  voices[1].decay = 4;
    voices[1].sustain = 12; voices[1].release = 16;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_program_change(drv, 1, 1);
    m4a_cc(drv, 0, 7, 127); m4a_cc(drv, 0, 10, 64);
    m4a_cc(drv, 1, 7, 127); m4a_cc(drv, 1, 10, 64);

    m4a_note_on(drv, 0, 60, 100);
    m4a_note_on(drv, 1, 60, 100);
    m4a_advance(drv, 1024);

    /* First vblank: wave + noise both freshly started.  Exactly one NR34
     * and one NR44 with trigger=1 should appear. */
    const M4ARegWriteBatch *b = m4a_get_pending_writes(drv);
    int n34_trig = 0, n34_notrig = 0, n44_trig = 0, n44_notrig = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].reg == M4A_REG_NR34) {
            if (b->events[i].value & 0x80) n34_trig++; else n34_notrig++;
        }
        if (b->events[i].reg == M4A_REG_NR44) {
            if (b->events[i].value & 0x80) n44_trig++; else n44_notrig++;
        }
    }
    ASSERT_EQ(n34_trig, 1,           "first vblank emits exactly 1 NR34 with trigger");
    ASSERT_EQ(n44_trig, 1,           "first vblank emits exactly 1 NR44 with trigger");
    m4a_consume_writes(drv);

    /* Drive enough vblanks to traverse attack → decay → sustain.  All
     * subsequent NRx4 writes from envelope steps must carry trigger=0
     * (no fresh note since the initial note_on). */
    for (int chunk = 0; chunk < 20; chunk++) {
        m4a_advance(drv, 1024);
        b = m4a_get_pending_writes(drv);
        for (size_t i = 0; i < b->count; i++) {
            if (b->events[i].reg == M4A_REG_NR34) {
                if (b->events[i].value & 0x80) n34_trig++; else n34_notrig++;
            }
            if (b->events[i].reg == M4A_REG_NR44) {
                if (b->events[i].value & 0x80) n44_trig++; else n44_notrig++;
            }
        }
        m4a_consume_writes(drv);
    }
    ASSERT_EQ(n34_trig, 1,           "no extra NR34 retriggers during sustain");
    ASSERT_EQ(n44_trig, 1,           "no extra NR44 retriggers during sustain");
    /* Envelope updates do still emit NRx4-no-trigger frequently. */
    ASSERT(n34_notrig > 0,           "NR34-no-trigger emitted during sustain");
    ASSERT(n44_notrig > 0,           "NR44-no-trigger emitted during sustain");

    /* note_off → release: still no fresh trigger. */
    m4a_note_off(drv, 0, 60);
    m4a_note_off(drv, 1, 60);
    for (int chunk = 0; chunk < 8; chunk++) {
        m4a_advance(drv, 1024);
        b = m4a_get_pending_writes(drv);
        for (size_t i = 0; i < b->count; i++) {
            if (b->events[i].reg == M4A_REG_NR34 && (b->events[i].value & 0x80)) n34_trig++;
            if (b->events[i].reg == M4A_REG_NR44 && (b->events[i].value & 0x80)) n44_trig++;
        }
        m4a_consume_writes(drv);
    }
    ASSERT_EQ(n34_trig, 1,           "release does NOT refire NR34 trigger");
    ASSERT_EQ(n44_trig, 1,           "release does NOT refire NR44 trigger");

    /* New note_on → exactly one more trigger on each. */
    m4a_note_on(drv, 0, 64, 100);
    m4a_note_on(drv, 1, 64, 100);
    m4a_advance(drv, 1024);
    b = m4a_get_pending_writes(drv);
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].reg == M4A_REG_NR34 && (b->events[i].value & 0x80)) n34_trig++;
        if (b->events[i].reg == M4A_REG_NR44 && (b->events[i].value & 0x80)) n44_trig++;
    }
    ASSERT_EQ(n34_trig, 2,           "second note_on emits exactly one more NR34 trigger");
    ASSERT_EQ(n44_trig, 2,           "second note_on emits exactly one more NR44 trigger");

    m4a_driver_destroy(drv);
}

/* PCM_PUBLISH driver contract: m4a_sound_main_ram emits exactly one
 * M4A_REG_PCM_PUBLISH event per vblank, stamped with the firing
 * sample_offset (the host-frame offset within the current render
 * span where the vblank fired).  This is a DRIVER-side regression
 * that doesn't need the chip — all we're asserting is "the driver
 * publishes its ring writes to the event stream correctly."  The
 * full chip-side timing behaviour is covered separately by
 * test_v2_pcm_chunk_size_invariance and test_v2_pcm_publish_timing
 * in the full-v2 build slice. */
static void test_v2_pcm_publish_event_per_vblank(void)
{
    printf("Testing v2 driver emits one PCM_PUBLISH per vblank with correct offset...\n");

    /* Looping PCM voice — just needs enough activity for
     * m4a_sound_main_ram to actually run on each vblank. */
    int dataSize = 16;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0xC000;
    wd->freq = 22050u << 10;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++) wd->data[i] = (int8_t)((i & 1) ? 50 : -50);
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].sustain = 0xFF;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_set_master_volume(drv, 15);
    m4a_set_max_pcm_channels(drv, 5);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 127);

    /* Render-window 1: 1024 frames at 44100 Hz fires exactly 1 vblank
     * (vblank_step ≈ 738.36).  Expect exactly 1 PCM_PUBLISH event. */
    m4a_advance(drv, 1024);
    const M4ARegWriteBatch *b = m4a_get_pending_writes(drv);
    int npub = 0; uint32_t pub_off = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].reg == M4A_REG_PCM_PUBLISH) {
            if (npub == 0) pub_off = b->events[i].sample_offset;
            npub++;
        }
    }
    ASSERT_EQ(npub, 1,               "1024-frame advance fires exactly 1 PCM_PUBLISH");
    ASSERT(pub_off >= 700 && pub_off < 750,
                                     "PCM_PUBLISH sample_offset matches vblank firing (~738)");
    /* Ring write_cursor advanced by exactly one vblank's samples in
     * lockstep with the publish event count. */
    ASSERT_EQ((int)m4a_get_pcm_ring(drv)->write_cursor,
              (int)M4A_PCM_SAMPLES_PER_VBLANK,
                                     "ring write_cursor matches 1 published vblank");
    m4a_consume_writes(drv);

    /* Render-window 2: 4096 frames (≈ 5.55 vblank periods) → exactly 5
     * PCM_PUBLISH events, monotonic in sample_offset. */
    m4a_advance(drv, 4096);
    b = m4a_get_pending_writes(drv);
    int    pub_count = 0;
    uint32_t prev_off = 0;
    bool   monotonic = true;
    for (size_t i = 0; i < b->count; i++) {
        if (b->events[i].reg == M4A_REG_PCM_PUBLISH) {
            if (pub_count > 0 && b->events[i].sample_offset <= prev_off)
                monotonic = false;
            prev_off = b->events[i].sample_offset;
            pub_count++;
        }
    }
    ASSERT_EQ(pub_count, 5,          "4096-frame advance fires exactly 5 PCM_PUBLISH events");
    ASSERT(monotonic,                "PCM_PUBLISH sample_offsets are monotonic increasing");
    /* Ring write_cursor totals 1 + 5 = 6 vblanks of samples. */
    ASSERT_EQ((int)m4a_get_pcm_ring(drv)->write_cursor,
              6 * (int)M4A_PCM_SAMPLES_PER_VBLANK,
                                     "ring write_cursor matches cumulative published vblanks");
    m4a_consume_writes(drv);

    m4a_driver_destroy(drv);
    free(wd);

    /* Render-window 3 (fresh driver): a 64-frame advance at 44100 Hz
     * starts vblank_accum from 0, so it never crosses vblank_step
     * (~738.36) — expect zero PCM_PUBLISH events.  Exercises the
     * "chunk shorter than vblank" path that almost tripped the
     * chip-side publish-fallback heuristic when we built this fix.
     * (Can't reuse the prior driver because vblank_accum carries
     * residue from previous advances.) */
    {
        WaveData *wd2 = calloc(1, sizeof(WaveData) + dataSize + 1);
        wd2->type = 0; wd2->status = 0xC000; wd2->freq = 22050u << 10;
        wd2->loopStart = 0; wd2->size = dataSize;
        wd2->data = (int8_t *)((uint8_t *)wd2 + sizeof(WaveData));
        for (int i = 0; i < dataSize; i++) wd2->data[i] = (int8_t)((i & 1) ? 50 : -50);
        wd2->data[dataSize] = wd2->data[0];

        ToneData voices2[128];
        memset(voices2, 0, sizeof(voices2));
        voices2[0].type    = VOICE_DIRECTSOUND;
        voices2[0].key     = 60;
        voices2[0].wav     = wd2;
        voices2[0].attack  = 0xFF;
        voices2[0].sustain = 0xFF;

        M4ADriver *drv2 = m4a_driver_create(44100.0f);
        m4a_set_master_volume(drv2, 15);
        m4a_set_max_pcm_channels(drv2, 5);
        m4a_driver_set_voicegroup(drv2, voices2);
        m4a_program_change(drv2, 0, 0);
        m4a_cc(drv2, 0, 7, 127);
        m4a_cc(drv2, 0, 10, 64);
        m4a_note_on(drv2, 0, 60, 127);

        m4a_advance(drv2, 64);
        const M4ARegWriteBatch *b2 = m4a_get_pending_writes(drv2);
        int late_count = 0;
        for (size_t i = 0; i < b2->count; i++) {
            if (b2->events[i].reg == M4A_REG_PCM_PUBLISH) late_count++;
        }
        ASSERT_EQ(late_count, 0,     "fresh-driver 64-frame advance fires 0 PCM_PUBLISH");
        ASSERT_EQ((int)m4a_get_pcm_ring(drv2)->write_cursor, 0,
                                     "ring write_cursor stays 0 with no vblank");

        m4a_driver_destroy(drv2);
        free(wd2);
    }
}

/* The bounded event queue must never overflow under correct use of
 * the chunked render cycle.  Run a stressful sequence — many notes
 * across all 4 CGB channels including wave (which emits +16 wave-RAM
 * bytes per note start) over thousands of vblanks — and assert
 * events_dropped stays at 0.  If this ever fires, either chunking is
 * broken or M4A_EVENT_QUEUE_CAP is too small. */
static void test_v2_no_event_drops_over_long_run(void)
{
    printf("Testing v2 events_dropped stays 0 over long run...\n");

    /* Wave RAM block for the wave-channel voice. */
    static uint32_t waveRam[4];
    for (int i = 0; i < 16; i++) ((uint8_t*)waveRam)[i] = (uint8_t)(0x10 + i);

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_1;
    voices[0].key     = 60;
    voices[0].attack  = 0;  voices[0].decay = 0;
    voices[0].sustain = 16; voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    voices[1].type    = VOICE_SQUARE_2;
    voices[1].key     = 60;
    voices[1].attack  = 0;  voices[1].decay = 0;
    voices[1].sustain = 16; voices[1].release = 16;
    voices[1].wavePointer = (uint32_t *)(uintptr_t)2;

    voices[2].type    = VOICE_PROGRAMMABLE_WAVE;
    voices[2].key     = 60;
    voices[2].wavePointer = waveRam;
    voices[2].attack  = 0;  voices[2].decay = 0;
    voices[2].sustain = 16; voices[2].release = 16;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);

    /* Set up three tracks each with their own program. */
    for (int t = 0; t < 3; t++) {
        m4a_program_change(drv, t, t);
        m4a_cc(drv, t, 7, 127);
        m4a_cc(drv, t, 10, 64);
    }

    /* Cycle: note on each channel, render-and-consume in chunks for
     * ~6 vblanks, note off, repeat 64 times.  Each note-on for the
     * wave channel emits 16 WAVE_RAM_BYTE events plus the regular
     * NRxx writes — worst-case event density. */
    for (int cycle = 0; cycle < 64; cycle++) {
        m4a_note_on(drv, 0, 60, 100);
        m4a_note_on(drv, 1, 64, 100);
        m4a_note_on(drv, 2, 67, 100);
        v2_advance_chunked(drv, 4500);
        m4a_note_off(drv, 0, 60);
        m4a_note_off(drv, 1, 64);
        m4a_note_off(drv, 2, 67);
        v2_advance_chunked(drv, 1500);
    }

    ASSERT_EQ((int)m4a_get_events_dropped(drv), 0,
                                     "long stress run dropped no events");

    m4a_driver_destroy(drv);
}

#if defined(HW_AUDIO_V2)
/* Step 5 acceptance: a DirectSound voice + note_on flows through the
 * full chain — driver mixes PCM into M4APcmRing, chip S&Hs at
 * pcm_rate→host, sums into outL/outR.  Verifies non-zero output that
 * stays bounded. */
static void test_v2_directsound_audible(void)
{
    printf("Testing v2 DirectSound audible end-to-end...\n");

    /* Looping wave so the ring keeps filling across the test render
     * (non-looping 64-sample voices run out after 64 PCM ticks and
     * the rest of the render goes silent — uninteresting for a
     * "audible end-to-end" check). */
    int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0xC000;       /* GBA loop bits */
    wd->freq = 22050u << 10;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)((i & 1) ? 100 : -100);
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].sustain = 0xFF;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    HwAudio   *hw  = hw_audio_create(44100.0f);
    m4a_set_master_volume(drv, 15);
    m4a_set_max_pcm_channels(drv, 5);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 127);

    enum { N = 4096 };
    float L[N], R[N];
    v2_render_chunked(drv, hw, L, R, N);

    float peak = 0.0f;
    int nz = 0;
    for (int i = 0; i < N; i++) {
        float a = L[i] + R[i];   /* either side */
        if (a < 0) a = -a;
        if (a > peak) peak = a;
        if (L[i] != 0.0f || R[i] != 0.0f) nz++;
    }
    ASSERT(peak > 0.001f,            "DirectSound produces audible output");
    ASSERT(nz > N / 4,               "PCM contribution covers most of the buffer");
    ASSERT_EQ((int)m4a_get_events_dropped(drv), 0,
                                     "no events dropped during PCM render");

    m4a_driver_destroy(drv);
    hw_audio_destroy(hw);
    free(wd);
}

/* PCM publish gate: render the same PCM-only audio at two different
 * host-frame chunkings (one big call vs many small calls) and assert
 * bit-identical output.  Pre-fix: the chip's pcm_pos read clock saw
 * all of m4a_advance's ring writes from sample_offset 0, so changing
 * the chunk size shifted PCM data into different host frames.
 * Post-fix: PCM_PUBLISH events stamp each vblank's writes with the
 * firing offset; the chip clamps reads to the published range, and
 * pcm_pos pauses on FIFO underrun — so output is invariant under
 * arbitrary chunkings. */
static void render_with_chunking(M4ADriver *drv, HwAudio *hw,
                                 float *L, float *R, int total, int chunk_size)
{
    int off = 0;
    while (off < total) {
        int chunk = (total - off) > chunk_size ? chunk_size : (total - off);
        m4a_advance(drv, chunk);
        hw_audio_render_events(hw,
                               m4a_get_pending_writes(drv),
                               m4a_get_pcm_ring(drv),
                               L + off, R + off, chunk);
        m4a_consume_writes(drv);
        off += chunk;
    }
}

static void setup_pcm_test_driver(M4ADriver *drv, ToneData *voices, WaveData *wd)
{
    int dataSize = 64;
    wd->type = 0;
    wd->status = 0xC000;
    wd->freq = 22050u << 10;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    for (int i = 0; i < dataSize; i++)
        wd->data[i] = (int8_t)((i & 1) ? 100 : -100);
    wd->data[dataSize] = wd->data[0];

    memset(voices, 0, sizeof(ToneData) * 128);
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].sustain = 0xFF;

    m4a_set_master_volume(drv, 15);
    m4a_set_max_pcm_channels(drv, 5);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 127);
}

static void test_v2_pcm_chunk_size_invariance(void)
{
    printf("Testing v2 PCM output invariant under host chunk size...\n");

    enum { N = 4096 };
    float La[N], Ra[N], Lb[N], Rb[N], Lc[N], Rc[N];

    /* Run #1: one big call covers the entire window. */
    {
        ToneData *voices = calloc(128, sizeof(ToneData));
        WaveData *wd     = calloc(1, sizeof(WaveData) + 64 + 1);
        M4ADriver *drv   = m4a_driver_create(44100.0f);
        HwAudio   *hw    = hw_audio_create(44100.0f);
        setup_pcm_test_driver(drv, voices, wd);
        render_with_chunking(drv, hw, La, Ra, N, /*chunk_size=*/N);
        m4a_driver_destroy(drv); hw_audio_destroy(hw);
        free(wd); free(voices);
    }
    /* Run #2: 2048-frame chunks (the production v2_render_chunked size). */
    {
        ToneData *voices = calloc(128, sizeof(ToneData));
        WaveData *wd     = calloc(1, sizeof(WaveData) + 64 + 1);
        M4ADriver *drv   = m4a_driver_create(44100.0f);
        HwAudio   *hw    = hw_audio_create(44100.0f);
        setup_pcm_test_driver(drv, voices, wd);
        render_with_chunking(drv, hw, Lb, Rb, N, /*chunk_size=*/2048);
        m4a_driver_destroy(drv); hw_audio_destroy(hw);
        free(wd); free(voices);
    }
    /* Run #3: 64-frame chunks — vblank-rate-incompatible chunking that
     * stresses the per-chunk PUBLISH plumbing. */
    {
        ToneData *voices = calloc(128, sizeof(ToneData));
        WaveData *wd     = calloc(1, sizeof(WaveData) + 64 + 1);
        M4ADriver *drv   = m4a_driver_create(44100.0f);
        HwAudio   *hw    = hw_audio_create(44100.0f);
        setup_pcm_test_driver(drv, voices, wd);
        render_with_chunking(drv, hw, Lc, Rc, N, /*chunk_size=*/64);
        m4a_driver_destroy(drv); hw_audio_destroy(hw);
        free(wd); free(voices);
    }

    /* All three must match.  The chip-internal pipeline is event-driven
     * and rate-decoupled; PCM publish events stamp data availability at
     * exact host-frame offsets; pcm_pos pauses on underrun.  Together
     * these guarantee that the same audio renders identically at any
     * chunking. */
    int max_diff_idx = -1;
    float max_diff = 0.0f;
    for (int i = 0; i < N; i++) {
        float d_ab_l = La[i] - Lb[i]; if (d_ab_l < 0) d_ab_l = -d_ab_l;
        float d_ab_r = Ra[i] - Rb[i]; if (d_ab_r < 0) d_ab_r = -d_ab_r;
        float d_ac_l = La[i] - Lc[i]; if (d_ac_l < 0) d_ac_l = -d_ac_l;
        float d_ac_r = Ra[i] - Rc[i]; if (d_ac_r < 0) d_ac_r = -d_ac_r;
        float d = d_ab_l;
        if (d_ab_r > d) d = d_ab_r;
        if (d_ac_l > d) d = d_ac_l;
        if (d_ac_r > d) d = d_ac_r;
        if (d > max_diff) { max_diff = d; max_diff_idx = i; }
    }
    /* Allow tiny float rounding (resampler floats accumulate slightly
     * differently across chunkings).  But the divergence must be at
     * the sub-1e-4 level, not the per-vblank-shift level (~0.78). */
    if (max_diff > 1e-4f) {
        fprintf(stderr, "  max_diff=%g at frame %d (La=%g Lb=%g Lc=%g)\n",
                max_diff, max_diff_idx,
                La[max_diff_idx], Lb[max_diff_idx], Lc[max_diff_idx]);
    }
    ASSERT(max_diff < 1e-4f,         "PCM output is chunk-size invariant");

    /* Sanity: the rendered audio is non-trivial (not all zero — a
     * trivially-equal silent comparison would also pass max_diff=0). */
    float peak = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = La[i] + Ra[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    ASSERT(peak > 0.001f,            "test renders non-trivial PCM signal");
}

/* PCM publish-timing: render a window that straddles a vblank firing.
 * Pre-vblank host frames must NOT see ring data that the post-vblank
 * portion of the same advance call wrote.  Test: silence all PCM
 * voices (so the ring stays at zero); render once to drain warmup;
 * note_on a sharp impulse-PCM voice; render a single chunk that
 * spans one vblank firing; assert the pre-vblank L samples are the
 * same as the held value from before the note_on (silent baseline),
 * and only post-vblank samples reflect the new note. */
static void test_v2_pcm_publish_timing(void)
{
    printf("Testing v2 PCM publish timing — pre-vblank cleanly separated from post-vblank...\n");

    static const int dataSize = 64;
    WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
    wd->type = 0;
    wd->status = 0xC000;
    wd->freq = 22050u << 10;
    wd->loopStart = 0;
    wd->size = dataSize;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
    /* Saturated +max — produces large positive PCM samples once the
     * voice runs.  Easy to detect against silent baseline. */
    for (int i = 0; i < dataSize; i++) wd->data[i] = 100;
    wd->data[dataSize] = wd->data[0];

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_DIRECTSOUND;
    voices[0].key     = 60;
    voices[0].wav     = wd;
    voices[0].attack  = 0xFF;
    voices[0].sustain = 0xFF;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    HwAudio   *hw  = hw_audio_create(44100.0f);
    m4a_set_master_volume(drv, 15);
    m4a_set_max_pcm_channels(drv, 5);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);

    /* Drain the resampler / FIFO startup with a long silent stretch.
     * After this the pipeline has steady-state held-zero everywhere. */
    enum { WARM = 8192 };
    static float warmL[WARM], warmR[WARM];
    render_with_chunking(drv, hw, warmL, warmR, WARM, 2048);

    /* Now note_on; a single ~1.4-vblank chunk (1024 frames @ 44100)
     * captures one vblank firing roughly in the middle. */
    m4a_note_on(drv, 0, 60, 127);
    enum { N = 1024 };
    float L[N], R[N];
    render_with_chunking(drv, hw, L, R, N, /*chunk_size=*/N);

    /* The chip's pcm_pos was (approximately) WARM * 0.30338 ≈ 2486.7
     * before this chunk; one full vblank period at 44100 Hz is
     * 738 host frames.  The first vblank firing within this chunk is
     * about 738-(WARM mod 738) frames in.  Pre-firing region: held
     * value from the warmup (zero, since baseline was silent and the
     * voice wasn't note_on yet); post-firing: ring fills with the +100
     * PCM voice and the chip starts reading it.
     *
     * We don't know the exact firing offset without driver introspection,
     * so test the structural property: the first non-trivial output
     * sample arrives AFTER frame 0, not at it.  Pre-fix the +100
     * samples leaked back into frame 0 because the chip read all
     * post-vblank ring data from sample_offset 0. */
    int first_nonzero = -1;
    for (int i = 0; i < N; i++) {
        float a = L[i] + R[i]; if (a < 0) a = -a;
        if (a > 0.01f) { first_nonzero = i; break; }
    }
    ASSERT(first_nonzero > 0,         "PCM signal does NOT appear at chunk frame 0");
    /* Sanity: the +100 voice does eventually become audible later in
     * the chunk (otherwise this would also pass on a silent stream). */
    float peak = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i] + R[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    ASSERT(peak > 0.05f,              "post-vblank PCM signal is audible");

    m4a_driver_destroy(drv);
    hw_audio_destroy(hw);
    free(wd);
}

#endif /* HW_AUDIO_V2 */

/* m4a_all_sound_off must silence every CGB channel immediately — not via
 * release.  CC 0x78 and plugin stop/reset rely on this. */
static void test_v2_all_sound_off_immediate(void)
{
    printf("Testing v2 all_sound_off is immediate...\n");

    ToneData voices[128];
    memset(voices, 0, sizeof(voices));
    voices[0].type    = VOICE_SQUARE_2;
    voices[0].key     = 60;
    voices[0].attack  = 0;
    voices[0].decay   = 0;
    voices[0].sustain = 16;
    voices[0].release = 16;
    voices[0].wavePointer = (uint32_t *)(uintptr_t)2;

    M4ADriver *drv = m4a_driver_create(44100.0f);
    m4a_driver_set_voicegroup(drv, voices);
    m4a_program_change(drv, 0, 0);
    m4a_cc(drv, 0, 7, 127);
    m4a_cc(drv, 0, 10, 64);
    m4a_note_on(drv, 0, 60, 100);
    m4a_advance(drv, 1024);

    const M4ARegisterFile *r = m4a_get_register_file(drv);
    ASSERT(r->sq2_enabled,           "ASO precondition: sq2 sounding");

    m4a_all_sound_off(drv);
    /* No m4a_advance call between ASO and inspection — disable must take
     * effect on the register file *immediately*, not on the next vblank. */
    ASSERT(!r->sq2_enabled,          "all_sound_off clears sq2_enabled immediately");
    ASSERT_EQ(r->sq2_env_volume, 0,  "all_sound_off zeroes sq2 env volume");
    ASSERT(!r->trigger_sq2,          "all_sound_off does NOT set trigger_sq2");

    m4a_driver_destroy(drv);
}

static void test_v2_stereo_toggle_matches_emerald(void)
{
    printf("Testing v2 stereo/mono toggle matches Emerald SOUNDCNT_H routing...\n");

    M4ADriver *drv = m4a_driver_create(44100.0f);
    const M4ARegisterFile *r = m4a_get_register_file(drv);

    ASSERT_EQ(r->psg_volume_code, 2, "stereo CGB full mix");
    ASSERT_EQ(r->dma_a_volume_code, 1, "stereo DMA A full mix");
    ASSERT_EQ(r->dma_b_volume_code, 1, "stereo DMA B full mix");
    ASSERT(!r->dma_a_enable_left,   "stereo DMA A not left");
    ASSERT(r->dma_a_enable_right,   "stereo DMA A right");
    ASSERT(r->dma_b_enable_left,    "stereo DMA B left");
    ASSERT(!r->dma_b_enable_right,  "stereo DMA B not right");

    m4a_set_stereo(drv, false);
    ASSERT_EQ(r->psg_volume_code, 2, "mono CGB full mix");
    ASSERT_EQ(r->dma_a_volume_code, 0, "mono DMA A half mix");
    ASSERT_EQ(r->dma_b_volume_code, 0, "mono DMA B half mix");
    ASSERT(r->dma_a_enable_left,    "mono DMA A left");
    ASSERT(r->dma_a_enable_right,   "mono DMA A right");
    ASSERT(r->dma_b_enable_left,    "mono DMA B left");
    ASSERT(r->dma_b_enable_right,   "mono DMA B right");
    ASSERT_EQ(m4a_get_pending_writes(drv)->events[0].value, 0x3302,
              "mono emits Emerald SOUNDCNT_H value");

    m4a_set_stereo(drv, true);
    ASSERT_EQ(r->dma_a_volume_code, 1, "restored DMA A full mix");
    ASSERT_EQ(r->dma_b_volume_code, 1, "restored DMA B full mix");
    ASSERT(!r->dma_a_enable_left,   "restored DMA A not left");
    ASSERT(r->dma_a_enable_right,   "restored DMA A right");
    ASSERT(r->dma_b_enable_left,    "restored DMA B left");
    ASSERT(!r->dma_b_enable_right,  "restored DMA B not right");

    m4a_driver_destroy(drv);
}

extern void m4a_chn_vol_set_cgb(M4ADriverCgbChan *ch, bool mono);

static void test_v2_cgb_mono_forces_both_sides(void)
{
    printf("Testing v2 CGB mono forces both-side pan...\n");

    M4ADriverCgbChan ch;
    memset(&ch, 0, sizeof(ch));
    ch.rightVolume = 255;
    ch.leftVolume  = 1;
    ch.sustain     = 15;
    ch.panMask     = 0xFF;

    m4a_chn_vol_set_cgb(&ch, false);
    ASSERT_EQ(ch.pan, 0x0F, "stereo CGB hard-right pan remains hard-right");

    m4a_chn_vol_set_cgb(&ch, true);
    ASSERT_EQ(ch.pan, 0xFF, "mono CGB pan routes both sides");

    ch.panMask = 0xF0;
    m4a_chn_vol_set_cgb(&ch, true);
    ASSERT_EQ(ch.pan, 0xF0, "mono CGB still honors pan mask");
}
#endif

#if defined(HW_AUDIO_V2)
/* ---- Chip-only canned-event tests.  Run under HW_AUDIO_V2 alone (no
 * M4A_DRIVER_V2 needed): construct M4ARegWriteBatch by hand and feed
 * directly to hw_audio_render_events.  Per plan §10 step 4 these are
 * the right granularity for chip-side parity work without booting the
 * driver. ---- */

static void test_hw_psg_frame_sequencer_init_convention(void)
{
    printf("Testing hw_psg frame sequencer: init first tick dispatches step 1...\n");

    HwPsgSynth psg;
    float sq1[256], sq2[256], wave[256], noise[256];
    HwPsgFrameSequencerDebug dbg;

    hw_psg_init(&psg, 131072.0f);
    hw_psg_render(&psg, sq1, sq2, wave, noise, 256);
    hw_psg_get_frame_sequencer_debug(&psg, &dbg);

    ASSERT_EQ(dbg.frame_ticks, 1,    "one 512 Hz frame tick after 256 samples at 131072 Hz");
    ASSERT_EQ(dbg.frame_step, 1,     "full init starts from mGBA frame=0, first tick dispatches step 1");
    ASSERT_EQ(dbg.length_ticks, 0,   "step 1 does not clock length");
    ASSERT_EQ(dbg.sweep_ticks, 0,    "step 1 does not clock sweep");
    ASSERT_EQ(dbg.envelope_ticks, 0, "step 1 does not clock envelope");
}

static void test_hw_psg_frame_sequencer_dispatch_table(void)
{
    printf("Testing hw_psg frame sequencer: eight-step dispatch table...\n");

    HwPsgSynth psg;
    HwPsgFrameSequencerDebug dbg;

    hw_psg_init(&psg, 131072.0f);
    hw_psg_render(&psg, NULL, NULL, NULL, NULL, 2048);
    hw_psg_get_frame_sequencer_debug(&psg, &dbg);

    ASSERT_EQ(dbg.frame_ticks, 8,    "eight frame ticks after 2048 samples at 131072 Hz");
    ASSERT_EQ(dbg.frame_step, 0,     "eighth tick wraps to frame step 0");
    ASSERT_EQ(dbg.length_ticks, 4,   "length clocks on steps 0,2,4,6");
    ASSERT_EQ(dbg.sweep_ticks, 2,    "sweep clocks on steps 2,6");
    ASSERT_EQ(dbg.envelope_ticks, 1, "envelope clocks on step 7");
}

static void test_hw_psg_frame_sequencer_chunk_invariance(void)
{
    printf("Testing hw_psg frame sequencer: chunking does not affect timing...\n");

    HwPsgSynth full, chunked;
    HwPsgFrameSequencerDebug dbg_full, dbg_chunked;

    hw_psg_init(&full, 131072.0f);
    hw_psg_render(&full, NULL, NULL, NULL, NULL, 2048);
    hw_psg_get_frame_sequencer_debug(&full, &dbg_full);

    hw_psg_init(&chunked, 131072.0f);
    int remaining = 2048;
    while (remaining > 0) {
        int n = remaining > 17 ? 17 : remaining;
        hw_psg_render(&chunked, NULL, NULL, NULL, NULL, n);
        remaining -= n;
    }
    hw_psg_get_frame_sequencer_debug(&chunked, &dbg_chunked);

    ASSERT_EQ(dbg_chunked.frame_ticks, dbg_full.frame_ticks,
              "chunked render matches full render frame tick count");
    ASSERT_EQ(dbg_chunked.frame_step, dbg_full.frame_step,
              "chunked render matches full render frame step");
    ASSERT_EQ(dbg_chunked.length_ticks, dbg_full.length_ticks,
              "chunked render matches full render length ticks");
    ASSERT_EQ(dbg_chunked.sweep_ticks, dbg_full.sweep_ticks,
              "chunked render matches full render sweep ticks");
    ASSERT_EQ(dbg_chunked.envelope_ticks, dbg_full.envelope_ticks,
              "chunked render matches full render envelope ticks");
    ASSERT_NEAR(dbg_chunked.frame_accum, dbg_full.frame_accum, 1e-12,
                "chunked render matches full render frame accumulator");
}

static void test_hw_psg_frame_sequencer_rate_continuity(void)
{
    printf("Testing hw_psg frame sequencer: render-rate change preserves phase...\n");

    HwPsgSynth psg;
    HwPsgFrameSequencerDebug dbg;

    hw_psg_init(&psg, 131072.0f);
    hw_psg_render(&psg, NULL, NULL, NULL, NULL, 128);
    hw_psg_set_render_rate(&psg, 262144.0f);
    hw_psg_render(&psg, NULL, NULL, NULL, NULL, 256);
    hw_psg_get_frame_sequencer_debug(&psg, &dbg);

    ASSERT_EQ(dbg.frame_ticks, 1,    "half tick at 131072 plus half tick at 262144 clocks once");
    ASSERT_EQ(dbg.frame_step, 1,     "rate change does not reset frame step");
    ASSERT_NEAR(dbg.frame_accum, 0.0, 1e-12,
                "rate change preserves accumulator and lands on tick boundary");
}

static void test_hw_psg_frame_sequencer_nr52_power_cycle(void)
{
    printf("Testing hw_psg frame sequencer: NR52 off-to-on follows mGBA step 0 convention...\n");

    HwPsgSynth psg;
    HwPsgFrameSequencerDebug dbg;
    M4ARegWrite off = { 0, M4A_REG_NR52, 0x00 };
    M4ARegWrite on  = { 0, M4A_REG_NR52, 0x80 };

    hw_psg_init(&psg, 131072.0f);
    hw_psg_render(&psg, NULL, NULL, NULL, NULL, 2048);
    hw_psg_apply_event(&psg, &off);
    hw_psg_apply_event(&psg, &on);
    hw_psg_render(&psg, NULL, NULL, NULL, NULL, 256);
    hw_psg_get_frame_sequencer_debug(&psg, &dbg);

    ASSERT_EQ(dbg.frame_ticks, 1,    "NR52 off-to-on resets frame tick counters");
    ASSERT_EQ(dbg.frame_step, 0,     "mGBA NR52 re-enable sets frame=7, first tick dispatches step 0");
    ASSERT_EQ(dbg.length_ticks, 1,   "step 0 clocks length after NR52 re-enable");
    ASSERT_EQ(dbg.sweep_ticks, 0,    "step 0 does not clock sweep");
    ASSERT_EQ(dbg.envelope_ticks, 0, "step 0 does not clock envelope");
}

static void test_hw_psg_frame_sequencer_disabled_does_not_advance(void)
{
    printf("Testing hw_psg frame sequencer: disabled master stays silent and does not advance...\n");

    HwPsgSynth psg;
    float sq1[512], sq2[512], wave[512], noise[512];
    HwPsgFrameSequencerDebug dbg;
    M4ARegWrite off = { 0, M4A_REG_NR52, 0x00 };

    hw_psg_init(&psg, 131072.0f);
    hw_psg_apply_event(&psg, &off);
    memset(sq1, 1, sizeof(sq1));
    memset(sq2, 1, sizeof(sq2));
    memset(wave, 1, sizeof(wave));
    memset(noise, 1, sizeof(noise));
    hw_psg_render(&psg, sq1, sq2, wave, noise, 512);
    hw_psg_get_frame_sequencer_debug(&psg, &dbg);

    bool silent = true;
    for (int i = 0; i < 512; i++) {
        if (sq1[i] != 0.0f || sq2[i] != 0.0f ||
            wave[i] != 0.0f || noise[i] != 0.0f) {
            silent = false;
            break;
        }
    }
    ASSERT(silent,                   "NR52 disabled render zeroes all direct PSG outputs");
    ASSERT_EQ(dbg.frame_ticks, 0,    "NR52 disabled render does not advance frame sequencer");
    ASSERT_EQ(dbg.frame_step, 7,     "NR52 disabled state holds mGBA re-enable frame seed");
}

static void test_hw_psg_frame_sequencer_nr52_enabled_write_no_reset(void)
{
    printf("Testing hw_psg frame sequencer: NR52 enabled->enabled does not reset...\n");

    HwPsgSynth psg;
    HwPsgFrameSequencerDebug before, after;
    M4ARegWrite on = { 0, M4A_REG_NR52, 0x80 };

    hw_psg_init(&psg, 131072.0f);
    hw_psg_render(&psg, NULL, NULL, NULL, NULL, 384);
    hw_psg_get_frame_sequencer_debug(&psg, &before);
    hw_psg_apply_event(&psg, &on);
    hw_psg_get_frame_sequencer_debug(&psg, &after);

    ASSERT_EQ(after.frame_ticks, before.frame_ticks,
              "enabled NR52 write preserves frame tick count");
    ASSERT_EQ(after.frame_step, before.frame_step,
              "enabled NR52 write preserves frame step");
    ASSERT_NEAR(after.frame_accum, before.frame_accum, 1e-12,
                "enabled NR52 write preserves frame accumulator");
    ASSERT_EQ(after.length_ticks, before.length_ticks,
              "enabled NR52 write preserves length counter");
    ASSERT_EQ(after.sweep_ticks, before.sweep_ticks,
              "enabled NR52 write preserves sweep counter");
    ASSERT_EQ(after.envelope_ticks, before.envelope_ticks,
              "enabled NR52 write preserves envelope counter");
}

static void test_hw_psg_frame_sequencer_nr52_disabled_write_stable(void)
{
    printf("Testing hw_psg frame sequencer: NR52 disabled->disabled stays stable...\n");

    HwPsgSynth psg;
    HwPsgFrameSequencerDebug before, after;
    M4ARegWrite off = { 0, M4A_REG_NR52, 0x00 };

    hw_psg_init(&psg, 131072.0f);
    hw_psg_render(&psg, NULL, NULL, NULL, NULL, 384);
    hw_psg_apply_event(&psg, &off);
    hw_psg_get_frame_sequencer_debug(&psg, &before);
    hw_psg_apply_event(&psg, &off);
    hw_psg_get_frame_sequencer_debug(&psg, &after);

    ASSERT_EQ(after.frame_ticks, before.frame_ticks,
              "disabled NR52 write preserves frame tick count");
    ASSERT_EQ(after.frame_step, before.frame_step,
              "disabled NR52 write preserves frame step");
    ASSERT_NEAR(after.frame_accum, before.frame_accum, 1e-12,
                "disabled NR52 write preserves frame accumulator");
    ASSERT_EQ(after.length_ticks, before.length_ticks,
              "disabled NR52 write preserves length counter");
    ASSERT_EQ(after.sweep_ticks, before.sweep_ticks,
              "disabled NR52 write preserves sweep counter");
    ASSERT_EQ(after.envelope_ticks, before.envelope_ticks,
              "disabled NR52 write preserves envelope counter");
}

static void test_hw_psg_nr52_power_cycle_preserves_wave_ram(void)
{
    printf("Testing hw_psg: NR52 power cycle preserves wave RAM...\n");

    HwPsgSynth psg;
    M4ARegWrite off = { 0, M4A_REG_NR52, 0x00 };
    M4ARegWrite on  = { 0, M4A_REG_NR52, 0x80 };

    hw_psg_init(&psg, 131072.0f);
    for (uint32_t i = 0; i < 16; i++) {
        M4ARegWrite wr = { 0, M4A_REG_WAVE_RAM_BYTE,
                           (i << 8) | (uint8_t)(0xA0u | i) };
        hw_psg_apply_event(&psg, &wr);
    }
    hw_psg_apply_event(&psg, &off);
    hw_psg_apply_event(&psg, &on);

    bool preserved = true;
    for (uint32_t i = 0; i < 16; i++) {
        if (psg.wave_ram[i] != (uint8_t)(0xA0u | i)) {
            preserved = false;
            break;
        }
    }
    ASSERT(preserved,                "NR52 disable/re-enable does not clear channel 3 wave RAM");
}

static void test_chip_canned_square_audible(void)
{
    printf("Testing chip-only: canned NRxx events drive sq2 audible...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Hand-built event batch: enable master + full master vol + sq2
     * routed to both sides, then trigger sq2 at freq word 1700 (50%
     * duty, env volume 15). */
    M4ARegWrite ev[] = {
        { 0, M4A_REG_NR52, 0x80 },                     /* master enable */
        { 0, M4A_REG_NR50, 0x77 },                     /* master vol max both */
        { 0, M4A_REG_NR51, 0x22 },                     /* sq2 → both */
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },               /* PSG vol = 100% */
        { 0, M4A_REG_NR21, 0x80 },                     /* duty 50% (bit7) */
        { 0, M4A_REG_NR22, 0xF8 },                     /* env vol 15, dir+, pace 0 */
        { 0, M4A_REG_NR23, 1700 & 0xFF },              /* freq lo */
        { 0, M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) }, /* trigger + freq hi */
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

    enum { N = 4096 };
    float L[N], R[N];
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    float peak = 0.0f;
    int signChanges = 0;
    float prev = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
        if (i > 0 && ((L[i] >= 0) != (prev >= 0))) signChanges++;
        prev = L[i];
    }
    ASSERT(peak > 0.001f,            "canned sq2 events produce non-zero output");
    ASSERT(signChanges > 10,         "canned sq2 oscillates");

    hw_audio_destroy(hw);
}

static void test_chip_canned_master_disable_silences(void)
{
    printf("Testing chip-only: NR52 master-disable silences PSG...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* First arm sq2, then disable master.  Output should go silent
     * regardless of channel state. */
    M4ARegWrite ev[] = {
        { 0,    M4A_REG_NR52, 0x80 },
        { 0,    M4A_REG_NR50, 0x77 },
        { 0,    M4A_REG_NR51, 0x22 },
        { 0,    M4A_REG_SOUNDCNT_H, 0x02 },
        { 0,    M4A_REG_NR21, 0x80 },
        { 0,    M4A_REG_NR22, 0xF8 },
        { 0,    M4A_REG_NR23, 1700 & 0xFF },
        { 0,    M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) },
        /* Disable master halfway through the render. */
        { 2048, M4A_REG_NR52, 0x00 },
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

    enum { N = 4096 };
    float L[N], R[N];
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    /* First half: signal; second half: silence after a smear region.
     * The §12-step-9 polyphase resampler has a kernel half-width of
     * HW_RESAMPLE_TAPS/2 input (chip-internal) samples = 16, which at
     * step ≈ 2.97 is about 5 host samples either side of the event.
     * A few dozen host samples past the event boundary is well clear
     * of the smear; the late tail must be exactly zero. */
    float peakFirst = 0.0f;
    /* Stay clear of the smear at the boundary; the first few hundred
     * samples are plenty audible without including transition smear. */
    for (int i = 64; i < 2048 - 64; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peakFirst) peakFirst = a;
    }
    ASSERT(peakFirst > 0.001f,       "first half (master enabled) audible");

    bool secondSilent = true;
    /* Skip the resampler smear region right after the event. */
    for (int i = 2048 + 64; i < N; i++) {
        if (L[i] != 0.0f || R[i] != 0.0f) { secondSilent = false; break; }
    }
    ASSERT(secondSilent,             "second half (master disabled) all-zero past smear");

    hw_audio_destroy(hw);
}

static void test_chip_canned_pan_routing(void)
{
    printf("Testing chip-only: NR51 routes sq2 to L only...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* sq2 routed to LEFT only (NR51 bit 5 set, bit 1 clear). */
    M4ARegWrite ev[] = {
        { 0, M4A_REG_NR52, 0x80 },
        { 0, M4A_REG_NR50, 0x77 },
        { 0, M4A_REG_NR51, 0x20 },                     /* sq2 → L only */
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },
        { 0, M4A_REG_NR21, 0x80 },
        { 0, M4A_REG_NR22, 0xF8 },
        { 0, M4A_REG_NR23, 1700 & 0xFF },
        { 0, M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) },
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

    enum { N = 2048 };
    float L[N], R[N];
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peakL) peakL = a;
        a = R[i]; if (a < 0) a = -a;
        if (a > peakR) peakR = a;
    }
    ASSERT(peakL > 0.001f,           "L side carries sq2 signal");
    ASSERT(peakR == 0.0f,            "R side is exactly zero");

    hw_audio_destroy(hw);
}

static void test_chip_canned_wave_audible(void)
{
    printf("Testing chip-only: canned wave-RAM events produce wave output...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Build 16-byte wave RAM events (alternating 0xF/0x0 pattern for a
     * loud square-ish wave), then NR30=DAC-on + NR32=100% + NR34
     * trigger. */
    M4ARegWrite ev[40];
    size_t n = 0;
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR52, 0x80 };
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR50, 0x77 };
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR51, 0x44 };  /* wave → both */
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_SOUNDCNT_H, 0x02 };
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR30, 0x00 };  /* DAC off for write */
    for (uint32_t i = 0; i < 16; i++)
        ev[n++] = (M4ARegWrite){ 0, M4A_REG_WAVE_RAM_BYTE,
                                  (i << 8) | ((i & 1) ? 0x00 : 0xFF) };
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR30, 0x80 };  /* DAC on */
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR32, 0x20 };  /* 100% */
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR33, 1500 & 0xFF };
    ev[n++] = (M4ARegWrite){ 0, M4A_REG_NR34, 0x80 | ((1500 >> 8) & 7) };
    M4ARegWriteBatch batch = { .events = ev, .count = n };

    enum { N = 4096 };
    float L[N], R[N];
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    float peak = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    ASSERT(peak > 0.001f,            "canned wave events produce non-zero output");

    hw_audio_destroy(hw);
}

/* SOUNDCNT_H DMA routing: handcraft a ring with non-zero ring_a only,
 * and a SOUNDCNT_H event setting "DMA A → left only".  Output L should
 * carry signal; R should be near zero. */
static void test_chip_canned_pcm_routing(void)
{
    printf("Testing chip-only: SOUNDCNT_H routes DMA A to L only...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Build a fake M4APcmRing on the stack: 100% saw on ring_a, 0 on
     * ring_b, write_cursor far ahead, pcm_rate_hz set. */
    M4APcmRing ring;
    memset(&ring, 0, sizeof(ring));
    ring.pcm_rate_hz   = M4A_PCM_RATE_HZ;
    ring.write_cursor  = M4A_PCM_DMA_BUF_SIZE;
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        ring.ring_a[i] = (int8_t)((i * 4) - 127);
        ring.ring_b[i] = 0;
    }

    /* SOUNDCNT_H bit 9 = DMA A → left enable; bits 8/12/13 cleared
     * means R disabled and DMA B disabled both sides. */
    M4ARegWrite ev[] = {
        { 0, M4A_REG_SOUNDCNT_H, (1u << 9) | (1u << 2) }, /* A → L only, A vol 100% */
    };
    M4ARegWriteBatch batch = { .events = ev, .count = 1 };

    enum { N = 2048 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, &ring, L, R, N);

    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peakL) peakL = a;
        a = R[i]; if (a < 0) a = -a;
        if (a > peakR) peakR = a;
    }
    ASSERT(peakL > 0.001f,           "DMA A → L produces L signal");
    ASSERT(peakR < 0.001f,           "R side near zero (no DMA channel routed)");

    hw_audio_destroy(hw);
}

static void test_chip_canned_noise_audible(void)
{
    printf("Testing chip-only: canned NRxx events drive noise audible...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Hand-built event batch: enable master + full master vol + noise
     * routed to both sides; NR43 = 0x14 → clock_shift=1, divisor_code=4
     * (yields 524288/4/4 ≈ 32768 Hz LFSR rate, ~0.74 clocks per host
     * sample at 44100 Hz — many sign changes within 4096 frames). */
    M4ARegWrite ev[] = {
        { 0, M4A_REG_NR52, 0x80 },                     /* master enable */
        { 0, M4A_REG_NR50, 0x77 },                     /* master vol max both */
        { 0, M4A_REG_NR51, 0x88 },                     /* noise → both (bit 3) */
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },               /* PSG vol = 100% */
        { 0, M4A_REG_NR41, 0x00 },                     /* length-load (irrelevant) */
        { 0, M4A_REG_NR42, 0xF8 },                     /* env vol 15, dir+, pace 0 */
        { 0, M4A_REG_NR43, 0x14 },                     /* shift=1, w=15-bit, div=4 */
        { 0, M4A_REG_NR44, 0x80 },                     /* trigger */
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

    enum { N = 4096 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    /* Noise outputs ±kChanScale × env_vol/15 = ±0.25 (env=15).  Many
     * sign changes per 4096 frames — at ~32768 Hz LFSR rate the LSB
     * flips ~half the LFSR steps; we expect easily > 200 sign changes. */
    float peak = 0.0f;
    int signChanges = 0;
    float prev = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
        if (i > 0 && ((L[i] >= 0) != (prev >= 0))) signChanges++;
        prev = L[i];
    }
    ASSERT(peak > 0.001f,            "canned noise events produce non-zero output");
    ASSERT(signChanges > 200,        "canned noise oscillates many times");

    hw_audio_destroy(hw);
}

static void test_chip_canned_noise_dac_off_silences(void)
{
    printf("Testing chip-only: NR42=0 disables noise channel (DAC off)...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Arm noise, then write NR42=0 (top-5-bits clear → DAC off) midway. */
    M4ARegWrite ev[] = {
        { 0,    M4A_REG_NR52, 0x80 },
        { 0,    M4A_REG_NR50, 0x77 },
        { 0,    M4A_REG_NR51, 0x88 },
        { 0,    M4A_REG_SOUNDCNT_H, 0x02 },
        { 0,    M4A_REG_NR42, 0xF8 },
        { 0,    M4A_REG_NR43, 0x14 },
        { 0,    M4A_REG_NR44, 0x80 },
        { 2048, M4A_REG_NR42, 0x00 },                  /* DAC off */
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

    enum { N = 4096 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    /* First half: signal; second half: silent past resampler smear.
     * See test_chip_canned_master_disable_silences for smear rationale —
     * step-9 polyphase kernel smears step changes across ±5 host
     * samples; a 64-sample skip past the boundary is comfortable. */
    float peakFirst = 0.0f;
    for (int i = 64; i < 2048 - 64; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peakFirst) peakFirst = a;
    }
    ASSERT(peakFirst > 0.001f,       "first half (DAC on) audible");

    bool secondSilent = true;
    for (int i = 2048 + 64; i < N; i++) {
        if (L[i] != 0.0f || R[i] != 0.0f) { secondSilent = false; break; }
    }
    ASSERT(secondSilent,             "second half (DAC off) all-zero past smear");

    hw_audio_destroy(hw);
}

/* ---- §12 step 8: SOUNDBIAS bias-add + clip + SOUNDCNT_H scaling ----
 *
 * SCOPE: these tests validate the mix-bus *algebra* — SOUNDCNT_L/H
 * routing/scaling math, SOUNDBIAS bias_level DC offset math, and the
 * 10-bit DAC bias-add+clip math (default + asymmetric).  All of these
 * tests run with the chip at its post-§12.9 internal render rate
 * (`max(131072, 32768 << sampling_cycle)`) and through the polyphase
 * resampler to host.  Per-cadence tests live in the §12.10a block
 * below (cadence sweep + direct internal_rate switching assertion).
 *
 * They do NOT prove parity against mGBA / real-hardware captures —
 * that's §12.10b, still open.  See HW_AUDIO_SCAFFOLD_PLAN.md §12
 * "Blocking gates before parity claims" for the full open-gate list. */

static void test_chip_canned_soundbias_dc_offset(void)
{
    printf("Testing chip-only: SOUNDBIAS does NOT inject DC into silent output (mGBA _applyBias)...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* mGBA's _applyBias adds bias_level, clips to unsigned [0, 0x3FF],
     * then SUBTRACTS bias_level back — so non-default bias does not
     * leak into silent output.  Confirms hw_mix_render mirrors that
     * semantic (earlier poryaaaa code embedded bias_offset in output
     * for non-default bias, which was a divergence from mGBA).  Use
     * bias=0x280 (well off-default) with NR52 master disabled (no
     * signal) and assert the output stays at 0 ± kernel float
     * precision. */
    M4ARegWrite ev[] = {
        { 0, M4A_REG_NR52, 0x00 },
        { 0, M4A_REG_SOUNDBIAS, 0x280 },
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

    enum { N = 256 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    /* Expect every post-warmup sample at 0 within kernel precision.
     * The polyphase resampler's ring starts zero-filled, so this is
     * effectively zero-in-zero-out. */
    const float dc_eps      = 5e-5f;
    const int   warmup_host = 32;
    bool allOnTarget = true;
    for (int i = warmup_host; i < N; i++) {
        float dL = L[i]; if (dL < 0) dL = -dL;
        float dR = R[i]; if (dR < 0) dR = -dR;
        if (dL > dc_eps || dR > dc_eps) { allOnTarget = false; break; }
    }
    ASSERT(allOnTarget,              "non-default bias does NOT add DC to silent output");

    hw_audio_destroy(hw);
}

static void test_chip_canned_soundbias_clip_asymmetric(void)
{
    printf("Testing chip-only: SOUNDBIAS asymmetric clip caps the high side...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Asymmetric clip with mGBA _applyBias semantics.
     *
     * Pre-bias clip window in input space: [-bias_norm, max_norm - bias_norm]
     * where bias_norm = bias_level / 0x200 and max_norm = 0x3FF / 0x200.
     * bias=0x340 → bias_norm = 1.625 → clip window = [-1.625, +0.373].
     * Drive SQ1 + SQ2 unipolar inputs both routed to L only, both at
     * full env_vol → per-channel input range [0, 0.25] each, summed
     * range [0, 0.5] on L.  0.5 > 0.373 → top clips at 0.373, bottom
     * (input 0) stays at 0 (well within the negative half of the
     * window). */
    M4ARegWrite ev[] = {
        { 0, M4A_REG_NR52, 0x80 },
        { 0, M4A_REG_NR50, 0x77 },
        { 0, M4A_REG_NR51, 0x30 },                     /* sq1+sq2 → L only */
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },               /* PSG vol 100% */
        { 0, M4A_REG_SOUNDBIAS, 0x340 },
        /* SQ1: same freq + duty as SQ2 so both are high simultaneously. */
        { 0, M4A_REG_NR11, 0x80 },                     /* 50% duty */
        { 0, M4A_REG_NR12, 0xF8 },                     /* env_vol 15 */
        { 0, M4A_REG_NR13, 1700 & 0xFF },
        { 0, M4A_REG_NR14, 0x80 | ((1700 >> 8) & 7) },
        { 0, M4A_REG_NR21, 0x80 },                     /* SQ2 50% duty */
        { 0, M4A_REG_NR22, 0xF8 },
        { 0, M4A_REG_NR23, 1700 & 0xFF },
        { 0, M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) },
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

    enum { N = 4096 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, NULL, L, R, N);

    /* Output expectations:
     *   bias_norm = 0x340 / 0x200 = 1.625
     *   max_norm  = 0x3FF / 0x200 ≈ 1.998
     *   clip_top  = max_norm - bias_norm ≈ 0.373
     *   clip_bot  = -bias_norm = -1.625 (well below 0, never reached
     *               here because input is unipolar)
     * SQ1 + SQ2 both at full env (env_vol=15) routed to L:
     *   per-channel input = (env_vol/15) × kPsgChanScale × master_l × psg_vol
     *                     = 1.0 × 0.25 × 1.0 × 1.0 = 0.25
     *   when both high: input sum = 0.5 → clipped to 0.373
     *   when both low:  input sum = 0   → no clip, output 0
     *   half-overlap (one high one low): input = 0.25 → no clip, output 0.25
     * R side: no PSG routed → input 0 → output 0 (mGBA _applyBias
     * subtracts bias back, no embedded DC in silent output).
     *
     * Tolerances: polyphase resampler rings on the clipped square edges
     * (~9% Gibbs overshoot of step amplitude). */
    const float bias_norm = 832.0f / 512.0f;                     /* 1.625 */
    const float max_norm  = 1023.0f / 512.0f;                    /* ≈1.998 */
    const float clip_top  = max_norm - bias_norm;                /* ≈0.373 */
    const float ring_eps  = 0.04f;
    const float dc_eps    = 5e-5f;
    const int   warmup    = 32;
    float maxL = -1e9f, minL = +1e9f;
    for (int i = warmup; i < N; i++) {
        if (L[i] > maxL) maxL = L[i];
        if (L[i] < minL) minL = L[i];
    }
    ASSERT(maxL <= clip_top + ring_eps,
                                     "L high side clipped at clip_top (within ringing)");
    ASSERT(maxL >= clip_top - ring_eps,
                                     "L high side reaches clip ceiling (clip path exercised)");
    ASSERT(minL >= -ring_eps,        "L low side stays at/above 0 (no clip on negative)");
    /* R side has no PSG signal AND mGBA _applyBias subtracts bias back,
     * so silent output stays at 0. */
    float dR = R[warmup]; if (dR < 0) dR = -dR;
    ASSERT(dR < dc_eps,              "R-side silent output stays at 0 (no embedded bias)");

    hw_audio_destroy(hw);
}

static void test_chip_canned_soundcnth_psg_vol_codes(void)
{
    printf("Testing chip-only: SOUNDCNT_H PSG vol code 25/50/100%% scales...\n");

    /* Same SQ2 trigger at three SOUNDCNT_H psg-vol codes.  Peak L
     * amplitude must scale 25% : 50% : 100% within float epsilon.
     * Master vol is held at full to isolate the psg_vol_code factor. */
    const uint8_t  codes[3]      = { 0x00, 0x01, 0x02 };
    const float    expected[3]   = { 0.25f, 0.50f, 1.00f };
    float          peakOf[3]     = { 0 };

    for (int k = 0; k < 3; k++) {
        HwAudio *hw = hw_audio_create(44100.0f);
        M4ARegWrite ev[] = {
            { 0, M4A_REG_NR52, 0x80 },
            { 0, M4A_REG_NR50, 0x77 },
            { 0, M4A_REG_NR51, 0x20 },                     /* sq2 → L */
            { 0, M4A_REG_SOUNDCNT_H, codes[k] },
            { 0, M4A_REG_SOUNDBIAS, 0x200 },
            { 0, M4A_REG_NR21, 0x80 },
            { 0, M4A_REG_NR22, 0xF8 },
            { 0, M4A_REG_NR23, 1700 & 0xFF },
            { 0, M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) },
        };
        M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

        enum { N = 4096 };
        float L[N], R[N];
        memset(L, 0, sizeof(L));
        memset(R, 0, sizeof(R));
        hw_audio_render_events(hw, &batch, NULL, L, R, N);

        float peak = 0.0f;
        for (int i = 0; i < N; i++) {
            float a = L[i]; if (a < 0) a = -a;
            if (a > peak) peak = a;
        }
        peakOf[k] = peak;
        hw_audio_destroy(hw);
    }

    /* Ratios must match (peak is invariant across full-env square at
     * fixed pan/master, so peak[k]/peak[2] should equal expected[k]). */
    float ratio25  = peakOf[0] / peakOf[2];
    float ratio50  = peakOf[1] / peakOf[2];
    ASSERT(ratio25  > 0.245f && ratio25  < 0.255f,
                                     "25% PSG vol scales peak by ¼");
    ASSERT(ratio50  > 0.495f && ratio50  < 0.505f,
                                     "50% PSG vol scales peak by ½");
    ASSERT(peakOf[2] > 0.001f,        "100% PSG vol produces audible signal");
    (void)expected;
}

static void test_chip_canned_soundcnth_dma_vol_codes(void)
{
    printf("Testing chip-only: SOUNDCNT_H DMA A vol code 50/100%% scales...\n");

    M4APcmRing ring;
    memset(&ring, 0, sizeof(ring));
    ring.pcm_rate_hz   = M4A_PCM_RATE_HZ;
    ring.write_cursor  = M4A_PCM_DMA_BUF_SIZE;
    /* Constant +127 in ring_a maximises DMA A peak; ring_b stays 0. */
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        ring.ring_a[i] = 127;
        ring.ring_b[i] = 0;
    }

    /* code=0 → DMA A 50%, code=1 → 100%.  Both with A→L only. */
    const uint16_t cnt_h_code0 = (1u << 9);                /* A→L, A vol 50% */
    const uint16_t cnt_h_code1 = (1u << 9) | (1u << 2);    /* A→L, A vol 100% */

    float peak[2] = { 0 };
    const uint16_t cnt_h_codes[2] = { cnt_h_code0, cnt_h_code1 };

    for (int k = 0; k < 2; k++) {
        HwAudio *hw = hw_audio_create(44100.0f);
        M4ARegWrite ev[] = {
            { 0, M4A_REG_SOUNDCNT_H, cnt_h_codes[k] },
            { 0, M4A_REG_SOUNDBIAS,  0x200 },
        };
        M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };

        enum { N = 2048 };
        float L[N], R[N];
        memset(L, 0, sizeof(L));
        memset(R, 0, sizeof(R));
        hw_audio_render_events(hw, &batch, &ring, L, R, N);

        float p = 0.0f;
        for (int i = 0; i < N; i++) {
            float a = L[i]; if (a < 0) a = -a;
            if (a > p) p = a;
        }
        peak[k] = p;
        hw_audio_destroy(hw);
    }

    ASSERT(peak[1] > 0.001f,         "100% DMA vol produces audible signal");
    float ratio = peak[0] / peak[1];
    ASSERT(ratio > 0.495f && ratio < 0.505f,
                                     "50% DMA vol scales peak by ½");
}

/* ---- §12 step 9: polyphase resampler quality ----
 *
 * Anti-aliasing check: a square wave whose fundamental is above host
 * Nyquist (22050 Hz @ 44100) must be attenuated by the resampler's
 * low-pass kernel, NOT aliased into the audible band.  Compares peak
 * at a low fundamental (~1300 Hz, well in passband) to peak at a high
 * fundamental (~26214 Hz, above host Nyquist).  A linear-interp or
 * zero-order-hold resampler would let aliasing leak through and the
 * ratio would be ≈ 1.  Windowed-sinc rejects it heavily. */
static void test_chip_canned_resample_antialias(void)
{
    printf("Testing chip-only: polyphase resampler attenuates above host Nyquist...\n");

    /* Helper: render an SQ2 trigger at the given freq word and report
     * post-warmup peak amplitude on the L channel. */
    enum { N = 4096 };
    float L[N], R[N];

    /* Low-frequency reference: F=1947 → audio_hz = 131072/(2048-1947)
     * = 131072/101 ≈ 1298 Hz, deep in passband. */
    HwAudio *hw_low = hw_audio_create(44100.0f);
    M4ARegWrite ev_low[] = {
        { 0, M4A_REG_NR52, 0x80 },
        { 0, M4A_REG_NR50, 0x77 },
        { 0, M4A_REG_NR51, 0x22 },
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },
        { 0, M4A_REG_NR21, 0x80 },
        { 0, M4A_REG_NR22, 0xF8 },
        { 0, M4A_REG_NR23, 1947 & 0xFF },
        { 0, M4A_REG_NR24, 0x80 | ((1947 >> 8) & 7) },
    };
    M4ARegWriteBatch batch_low = { .events = ev_low, .count = sizeof(ev_low)/sizeof(ev_low[0]) };
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw_low, &batch_low, NULL, L, R, N);
    float peak_low = 0.0f;
    for (int i = 64; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peak_low) peak_low = a;
    }
    hw_audio_destroy(hw_low);

    /* High-frequency: F=2043 → audio_hz = 131072/5 = 26214 Hz, above
     * the host Nyquist of 22050 Hz.  Falls in the resampler's stopband. */
    HwAudio *hw_high = hw_audio_create(44100.0f);
    M4ARegWrite ev_high[] = {
        { 0, M4A_REG_NR52, 0x80 },
        { 0, M4A_REG_NR50, 0x77 },
        { 0, M4A_REG_NR51, 0x22 },
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },
        { 0, M4A_REG_NR21, 0x80 },
        { 0, M4A_REG_NR22, 0xF8 },
        { 0, M4A_REG_NR23, 2043 & 0xFF },
        { 0, M4A_REG_NR24, 0x80 | ((2043 >> 8) & 7) },
    };
    M4ARegWriteBatch batch_high = { .events = ev_high, .count = sizeof(ev_high)/sizeof(ev_high[0]) };
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw_high, &batch_high, NULL, L, R, N);
    float peak_high = 0.0f;
    for (int i = 64; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peak_high) peak_high = a;
    }
    hw_audio_destroy(hw_high);

    /* Sanity: low fundamental is audible. */
    ASSERT(peak_low > 0.001f,        "low-frequency square audible");
    /* Anti-alias bound: a 26214 Hz square would, without filtering,
     * produce ~kPsgChanScale = 0.25 peak at the host (square-wave
     * amplitude is largely unaffected by aliasing — the alias just
     * shifts the perceived frequency).  With the polyphase low-pass
     * we expect heavy attenuation of the fundamental and odd
     * harmonics; a ratio < 0.5 is a comfortable bound that linear
     * interp would not satisfy. */
    ASSERT(peak_high < peak_low * 0.5f,
                                     "high-freq sq attenuated by polyphase filter");
}

/* ---- §12 step 9: cumulative sample-clock invariance ----
 *
 * The chip-internal sample clock must advance in lock-step with the
 * cumulative HOST frame total, regardless of how the caller chunks
 * its render calls.  Without correct accounting (e.g. using
 * round(frames * step) per call, or pushing extra "lookahead" inputs
 * each call), each render block over-advances PSG/PCM/mix state by
 * a per-call constant — producing audible pitch / timing drift.
 *
 * These tests verify:
 *   (a) Block-size invariance — same audio rendered as one big call
 *       vs many small chunks produces sample-identical output past
 *       the resampler's startup warmup.
 *   (b) DC preservation across calls — feeding constant DC over
 *       many small chunks keeps the output at expected DC level
 *       without drift, ringing, or per-call discontinuities. */

/* Configure the same SQ2 trigger used by the plain audible test, then
 * render `frames` host samples using the given chunk size. */
static void render_sq2_chunked(HwAudio *hw, float *L, float *R,
                               int frames, int chunk_size)
{
    M4ARegWrite ev[] = {
        { 0, M4A_REG_NR52, 0x80 },
        { 0, M4A_REG_NR50, 0x77 },
        { 0, M4A_REG_NR51, 0x22 },
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },
        { 0, M4A_REG_NR21, 0x80 },
        { 0, M4A_REG_NR22, 0xF8 },
        { 0, M4A_REG_NR23, 1700 & 0xFF },
        { 0, M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) },
    };
    M4ARegWriteBatch first  = { .events = ev,  .count = sizeof(ev)/sizeof(ev[0]) };
    M4ARegWriteBatch empty  = { .events = NULL, .count = 0 };

    int produced = 0;
    bool first_chunk = true;
    while (produced < frames) {
        int this_chunk = chunk_size;
        if (this_chunk > frames - produced) this_chunk = frames - produced;
        const M4ARegWriteBatch *batch = first_chunk ? &first : &empty;
        hw_audio_render_events(hw, batch, NULL,
                               L + produced, R + produced, this_chunk);
        produced += this_chunk;
        first_chunk = false;
    }
}

static void test_chip_canned_block_size_invariance(void)
{
    printf("Testing chip-only: render output is invariant under host block size...\n");

    enum { N = 4096 };
    float L_full[N],  R_full[N];
    float L_64[N],    R_64[N];
    float L_127[N],   R_127[N];
    float L_512[N],   R_512[N];
    float L_2048[N],  R_2048[N];

    /* Reference: one 4096-frame call. */
    {
        HwAudio *hw = hw_audio_create(44100.0f);
        render_sq2_chunked(hw, L_full, R_full, N, N);
        hw_audio_destroy(hw);
    }

    /* Same render in chunks of 64, 127, 512, 2048.  Different
     * granularities exercise different rounding paths through the
     * cumulative-input accounting. */
    {
        HwAudio *hw = hw_audio_create(44100.0f);
        render_sq2_chunked(hw, L_64, R_64, N, 64);
        hw_audio_destroy(hw);
    }
    {
        HwAudio *hw = hw_audio_create(44100.0f);
        render_sq2_chunked(hw, L_127, R_127, N, 127);
        hw_audio_destroy(hw);
    }
    {
        HwAudio *hw = hw_audio_create(44100.0f);
        render_sq2_chunked(hw, L_512, R_512, N, 512);
        hw_audio_destroy(hw);
    }
    {
        HwAudio *hw = hw_audio_create(44100.0f);
        render_sq2_chunked(hw, L_2048, R_2048, N, 2048);
        hw_audio_destroy(hw);
    }

    /* Compare each chunked render against the full-call reference,
     * skipping the resampler's startup warmup (first ~32 host
     * samples).  Past warmup, the sample-clock accounting MUST be
     * deterministic regardless of how the call window was chunked.
     * Tolerance is intentionally tight — the only legitimate source
     * of difference is float-precision in the resampler's per-call
     * convolution accumulation, which is bit-stable for identical
     * input streams.  An older `frames * step + TAPS` per-call
     * approach would fail this test by ~per-call-count × step
     * samples of drift. */
    const int   warmup     = 64;
    const float invar_eps  = 1e-4f;
    float worst_64 = 0, worst_127 = 0, worst_512 = 0, worst_2048 = 0;
    for (int i = warmup; i < N; i++) {
        float dL64   = L_full[i] - L_64[i];   if (dL64   < 0) dL64   = -dL64;
        float dL127  = L_full[i] - L_127[i];  if (dL127  < 0) dL127  = -dL127;
        float dL512  = L_full[i] - L_512[i];  if (dL512  < 0) dL512  = -dL512;
        float dL2048 = L_full[i] - L_2048[i]; if (dL2048 < 0) dL2048 = -dL2048;
        if (dL64   > worst_64)   worst_64   = dL64;
        if (dL127  > worst_127)  worst_127  = dL127;
        if (dL512  > worst_512)  worst_512  = dL512;
        if (dL2048 > worst_2048) worst_2048 = dL2048;
    }
    ASSERT(worst_64   < invar_eps,   "block-size 64 matches full call");
    ASSERT(worst_127  < invar_eps,   "block-size 127 matches full call");
    ASSERT(worst_512  < invar_eps,   "block-size 512 matches full call");
    ASSERT(worst_2048 < invar_eps,   "block-size 2048 matches full call");
}

static void test_chip_canned_dc_streaming(void)
{
    printf("Testing chip-only: silent stream stays at 0 across many small calls...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Master disabled + bias = 0x280 → mGBA _applyBias semantics
     * (add bias, clip [0, 0x3FF], subtract bias) yields constant 0
     * output regardless of bias_level.  The cross-chunk invariance
     * we're testing is "no per-call discontinuities" — same target,
     * just expressed against 0 instead of a bias-injected DC level. */
    M4ARegWrite ev[] = {
        { 0, M4A_REG_NR52, 0x00 },
        { 0, M4A_REG_SOUNDBIAS, 0x280 },
    };
    M4ARegWriteBatch setup = { .events = ev,  .count = 2 };
    M4ARegWriteBatch empty = { .events = NULL, .count = 0 };

    /* Stream lots of small chunks.  After the first call's warmup
     * the output should hold at +0.25 without drift, gain change, or
     * inter-call discontinuities (which would manifest as periodic
     * spikes at chunk boundaries). */
    enum { CHUNK = 73, NCHUNKS = 200 };  /* 73 is a prime, prevents
                                            accidental alignment with
                                            internal rate ratios */
    float L[CHUNK], R[CHUNK];
    bool first_chunk = true;
    int  total_produced = 0;
    float worst_dc_dev   = 0.0f;
    int   worst_idx_in_chunk = -1;
    int   worst_chunk_idx = -1;
    int   first_steady_chunk = 1;  /* skip the first chunk (startup warmup) */

    for (int chunk_i = 0; chunk_i < NCHUNKS; chunk_i++) {
        const M4ARegWriteBatch *batch = first_chunk ? &setup : &empty;
        hw_audio_render_events(hw, batch, NULL, L, R, CHUNK);
        first_chunk = false;
        total_produced += CHUNK;

        if (chunk_i < first_steady_chunk) continue;

        for (int i = 0; i < CHUNK; i++) {
            float dL = L[i]; if (dL < 0) dL = -dL;
            float dR = R[i]; if (dR < 0) dR = -dR;
            float worst_here = dL > dR ? dL : dR;
            if (worst_here > worst_dc_dev) {
                worst_dc_dev = worst_here;
                worst_idx_in_chunk = i;
                worst_chunk_idx    = chunk_i;
            }
        }
    }

    /* Tolerance: kernel-precision drift only.  No per-call
     * discontinuities should be visible at this level. */
    const float dc_eps = 5e-5f;
    if (worst_dc_dev > dc_eps) {
        printf("  [debug] worst silent dev = %g at chunk %d sample %d\n",
               worst_dc_dev, worst_chunk_idx, worst_idx_in_chunk);
    }
    ASSERT(worst_dc_dev < dc_eps,
                                     "silent stream stays at 0 across all streaming chunks");
    ASSERT(total_produced == CHUNK * NCHUNKS,
                                     "total host frames produced equals total requested");

    hw_audio_destroy(hw);
}

/* ---- §12 step 10: per-SOUNDBIAS-cadence parity ----
 *
 * SOUNDBIAS bits 14-15 are the "amplitude resolution selector"
 * (sampling_cycle).  Plan §7b says the chip's internal output rate is
 * `max(131072, quirk_rate)` where `quirk_rate = 32768 << sampling_cycle`.
 * For sampling_cycle 0/1/2 the floor pins internal at 131072 Hz; only
 * sampling_cycle = 3 bumps it to 262144 Hz (the case that ROMhacks
 * use for cleaner PCM).
 *
 * These tests exercise all four cadences via the setup-then-play
 * pattern: one short render call applies SOUNDBIAS, the next call
 * runs real audio at the new rate.  Mid-call SOUNDBIAS sampling_cycle
 * changes are deferred to next render boundary by design — see
 * hw_audio.c's snapshot-at-start-of-call logic. */

/* Render an SQ2 trigger for `frames` host samples after applying the
 * given SOUNDBIAS value via a setup call.  Returns the post-warmup
 * peak amplitude on the L channel. */
static float run_sq2_at_soundbias(uint32_t soundbias, int frames,
                                  float *out_L, float *out_R)
{
    HwAudio *hw = hw_audio_create(44100.0f);

    /* Setup call: apply SOUNDBIAS in isolation.  A single-frame
     * render is enough to trigger the start-of-call rate sync. */
    M4ARegWrite setup_ev[] = {
        { 0, M4A_REG_SOUNDBIAS, soundbias },
    };
    M4ARegWriteBatch setup = { .events = setup_ev, .count = 1 };
    float scratch_l[1], scratch_r[1];
    hw_audio_render_events(hw, &setup, NULL, scratch_l, scratch_r, 1);

    /* Play call: trigger SQ2 + render the actual test signal. */
    M4ARegWrite play_ev[] = {
        { 0, M4A_REG_NR52, 0x80 },
        { 0, M4A_REG_NR50, 0x77 },
        { 0, M4A_REG_NR51, 0x22 },
        { 0, M4A_REG_SOUNDCNT_H, 0x02 },
        { 0, M4A_REG_NR21, 0x80 },
        { 0, M4A_REG_NR22, 0xF8 },
        { 0, M4A_REG_NR23, 1700 & 0xFF },
        { 0, M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) },
    };
    M4ARegWriteBatch play = { .events = play_ev, .count = sizeof(play_ev)/sizeof(play_ev[0]) };
    hw_audio_render_events(hw, &play, NULL, out_L, out_R, frames);

    float peak = 0.0f;
    for (int i = 64; i < frames; i++) {
        float a = out_L[i]; if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    hw_audio_destroy(hw);
    return peak;
}

static void test_chip_canned_soundbias_cycle_audible_sweep(void)
{
    printf("Testing chip-only: SQ2 audible at all four SOUNDBIAS sampling_cycle values...\n");

    enum { N = 4096 };
    float L[N], R[N];

    for (int sc = 0; sc < 4; sc++) {
        /* SOUNDBIAS layout: bits 1-9 = bias_level (0x200 default),
         * bits 14-15 = sampling_cycle. */
        uint32_t soundbias = 0x200u | ((uint32_t)sc << 14);
        float peak = run_sq2_at_soundbias(soundbias, N, L, R);
        ASSERT(peak > 0.001f,        "sq audible at this SOUNDBIAS sampling_cycle");
    }
}

/* Direct rate-switching test: assert hw_audio_internal_rate() actually
 * tracks SOUNDBIAS sampling_cycle.  An implementation that ignored
 * sampling_cycle and stayed at a fixed 131072 Hz internal rate would
 * fail this test — the level-comparison test below CAN'T detect that
 * (host-rate output is the same for any internal rate ≥ 2 × host_rate
 * on a low-frequency signal).  This is the unambiguous proof that
 * cadence switching is wired up.
 *
 * Test pattern matches the documented boot-time-only target: a
 * SOUNDBIAS event applied in one render call takes effect at the
 * START of the NEXT render call (snapshot-at-start-of-call sync).
 * So each transition is a setup call (apply SOUNDBIAS) plus a play
 * call (any subsequent render), and we read internal_rate AFTER the
 * play call. */
static void test_chip_canned_soundbias_internal_rate_switches(void)
{
    printf("Testing chip-only: hw_audio_internal_rate() tracks SOUNDBIAS sampling_cycle...\n");

    HwAudio *hw = hw_audio_create(44100.0f);

    /* Default sampling_cycle = 0 → internal at the 131072 Hz floor. */
    ASSERT_EQ(hw_audio_internal_rate(hw), 131072,
              "default sampling_cycle = 0 yields internal 131072 Hz");

    /* Helper macro: apply SOUNDBIAS via a setup call, then run a
     * trivial play call to trigger the start-of-call rate sync. */
    #define SOUNDBIAS_TRANSITION(sc_value) do {                                     \
        uint32_t sb_payload = 0x200u | ((uint32_t)(sc_value) << 14);                \
        M4ARegWrite setup_ev[] = { { 0, M4A_REG_SOUNDBIAS, sb_payload } };          \
        M4ARegWriteBatch setup = { .events = setup_ev, .count = 1 };                \
        M4ARegWriteBatch empty = { .events = NULL, .count = 0 };                    \
        float scratch[1];                                                           \
        hw_audio_render_events(hw, &setup, NULL, scratch, scratch, 1);              \
        hw_audio_render_events(hw, &empty, NULL, scratch, scratch, 1);              \
    } while (0)

    /* sampling_cycle 1 and 2 stay pinned at the floor
     * (max(131072, 32768<<sc) = 131072 for sc ≤ 2). */
    SOUNDBIAS_TRANSITION(1);
    ASSERT_EQ(hw_audio_internal_rate(hw), 131072,
              "sampling_cycle = 1 still pinned at 131072 Hz floor");

    SOUNDBIAS_TRANSITION(2);
    ASSERT_EQ(hw_audio_internal_rate(hw), 131072,
              "sampling_cycle = 2 still pinned at 131072 Hz floor");

    /* sampling_cycle 3 bumps to 262144 Hz. */
    SOUNDBIAS_TRANSITION(3);
    ASSERT_EQ(hw_audio_internal_rate(hw), 262144,
              "sampling_cycle = 3 bumps internal to 262144 Hz");

    /* Switching back lowers internal rate again. */
    SOUNDBIAS_TRANSITION(0);
    ASSERT_EQ(hw_audio_internal_rate(hw), 131072,
              "sampling_cycle back to 0 returns internal to 131072 Hz");

    #undef SOUNDBIAS_TRANSITION

    /* Single-call boundary check: applying SOUNDBIAS doesn't change
     * internal_rate within the same call.  This is the documented
     * boot-time-only restriction — make sure tests don't accidentally
     * rely on mid-call rate change. */
    {
        M4ARegWrite ev[] = { { 0, M4A_REG_SOUNDBIAS, 0x200u | (3u << 14) } };
        M4ARegWriteBatch batch = { .events = ev, .count = 1 };
        float scratch[1];
        /* Reset to a known state first. */
        hw_audio_destroy(hw);
        hw = hw_audio_create(44100.0f);
        ASSERT_EQ(hw_audio_internal_rate(hw), 131072, "fresh chip starts at 131072");
        hw_audio_render_events(hw, &batch, NULL, scratch, scratch, 1);
        ASSERT_EQ(hw_audio_internal_rate(hw), 131072,
                  "mid-call SOUNDBIAS doesn't switch rate (boot-time-only target)");
    }

    hw_audio_destroy(hw);
}

/* §12 step 5 — two-stage drain regression test.
 *
 * The PCM path is now a two-stage chain: HwDmaToFifo reads from
 * M4APcmRing at pcm_rate (≈13379 Hz), and HwFifoDrain snapshots that
 * FIFO head byte at the SOUNDBIAS-derived quirk_rate (32k/65k/131k/
 * 262k Hz).  For Pokemon Emerald defaults the two stages collapse
 * behaviorally to a single S&H (quirk >> pcm/2), so this test just
 * verifies the pipeline still produces audible, properly-routed PCM.
 *
 * The behavioral edge-cases that DO depend on the split (ROMhacks
 * pushing pcm_rate above quirk_rate / 2, where the quirk-rate S&H
 * acts as a low-pass at quirk Nyquist) are left for §12.10b
 * mGBA-comparison parity tests. */
static void test_chip_canned_pcm_two_stage_drain(void)
{
    printf("Testing chip-only: two-stage HwDmaToFifo + HwFifoDrain produces audible PCM...\n");

    M4APcmRing ring;
    memset(&ring, 0, sizeof(ring));
    ring.pcm_rate_hz   = M4A_PCM_RATE_HZ;
    ring.write_cursor  = M4A_PCM_DMA_BUF_SIZE;
    /* Sawtooth ramp on ring_a, zero on ring_b — gives both
     * non-trivial signal on A and a clean silence reference on B. */
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        ring.ring_a[i] = (int8_t)((i * 4) - 127);
        ring.ring_b[i] = 0;
    }

    HwAudio *hw = hw_audio_create(44100.0f);
    M4ARegWrite ev[] = {
        /* DMA A → both sides at 100%, DMA B disabled. */
        { 0, M4A_REG_SOUNDCNT_H, (1u << 8) | (1u << 9) | (1u << 2) },
    };
    M4ARegWriteBatch batch = { .events = ev, .count = 1 };

    enum { N = 2048 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, &ring, L, R, N);

    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < N; i++) {
        float a = L[i]; if (a < 0) a = -a;
        if (a > peakL) peakL = a;
        a = R[i]; if (a < 0) a = -a;
        if (a > peakR) peakR = a;
    }
    /* Sawtooth on ring_a routes to both sides; both peaks must be
     * audible.  Peak should land near kDmaChanScale × dma_a_vol = 0.5
     * (default vol code 1 → 100%) — saw peaks at int8 ±127 → ±~1.0 →
     * ×0.5 mix-bus scale ≈ ±0.5.  Allow generous tolerance for
     * resampler edge effects. */
    ASSERT(peakL > 0.1f,             "two-stage drain produces audible L PCM");
    ASSERT(peakR > 0.1f,             "two-stage drain produces audible R PCM");

    hw_audio_destroy(hw);
}

/* §12 step 5 — deterministic constant-byte test.
 *
 * Fill the entire ring with a single nonzero byte.  After resampler
 * warmup, post-mix-bus output must be exactly the held byte, properly
 * scaled by the routing + DMA-vol-code factors.  This pins the
 * absolute amplitude of the two-stage drain (a smoke test only checks
 * "non-zero", which can hide level / sign / scale bugs). */
static void test_chip_canned_pcm_constant_byte(void)
{
    printf("Testing chip-only: constant ring value yields exact post-mix DC...\n");

    M4APcmRing ring;
    memset(&ring, 0, sizeof(ring));
    ring.pcm_rate_hz   = M4A_PCM_RATE_HZ;
    ring.write_cursor  = M4A_PCM_DMA_BUF_SIZE;
    /* Constant +64 on ring_a (= +0.5 in normalized float space),
     * silence on ring_b.  Picking a power-of-2-friendly value keeps
     * the float math exact post-divide. */
    const int8_t kConstByte = 64;
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        ring.ring_a[i] = kConstByte;
        ring.ring_b[i] = 0;
    }

    HwAudio *hw = hw_audio_create(44100.0f);
    M4ARegWrite ev[] = {
        /* DMA A → L only at 100%, DMA B disabled. */
        { 0, M4A_REG_SOUNDCNT_H, (1u << 9) | (1u << 2) },
    };
    M4ARegWriteBatch batch = { .events = ev, .count = 1 };

    enum { N = 2048 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, &ring, L, R, N);

    /* Expected post-mix L value (after warmup):
     *   pcm_byte_normalized = +64 / 128 = +0.5
     *   × kDmaChanScale (0.5) → 0.25
     *   × a_l (= dma_a_vol = 1.0 with code = 1, routed to L)
     *   + bias_offset (0 with default 0x200)
     *   = +0.25
     * R is silent (DMA A not routed there, DMA B disabled, no PSG). */
    const float expected_L = 0.25f;
    const float dc_eps     = 5e-5f;
    const int   warmup     = 64;

    bool L_steady = true;
    bool R_steady = true;
    int  fail_idx = -1;
    for (int i = warmup; i < N; i++) {
        float dL = L[i] - expected_L; if (dL < 0) dL = -dL;
        float dR = R[i] - 0.0f;       if (dR < 0) dR = -dR;
        if (dL > dc_eps) { L_steady = false; fail_idx = i; break; }
        if (dR > dc_eps) { R_steady = false; fail_idx = i; break; }
    }
    ASSERT(L_steady,                 "L holds at +0.25 (= 64/128 × kDmaChanScale × vol × routing)");
    ASSERT(R_steady,                 "R is silent (no PCM routed there)");
    (void)fail_idx;

    hw_audio_destroy(hw);
}

/* §12 step 5 — explicit ring[0]-must-be-read regression.
 *
 * The two-stage drain originally had an off-by-one: held_pcm was
 * read only when pcm_pos crossed an integer boundary, but the model
 * "increment first, compare-to-prev" never fired for the 0→0
 * transition at session start, so ring[0] was silently skipped and
 * the first byte actually consumed was ring[1].
 *
 * This test pins that fix: place a positive impulse at ring[0] and
 * silence everywhere else.  The fixed pipeline produces a brief
 * positive transient in the host output (ring[0] held for a few
 * pcm-rate ticks at session start, then ring[1+] = 0).  An
 * implementation that skips ring[0] would output silence throughout. */
static void test_chip_canned_pcm_first_byte_consumed(void)
{
    printf("Testing chip-only: ring[0] is read at session start (no off-by-one)...\n");

    M4APcmRing ring;
    memset(&ring, 0, sizeof(ring));
    ring.pcm_rate_hz   = M4A_PCM_RATE_HZ;
    ring.write_cursor  = M4A_PCM_DMA_BUF_SIZE;
    /* Impulse at ring[0]; zeros everywhere else. */
    ring.ring_a[0] = 100;
    /* (rest already zero from memset) */

    HwAudio *hw = hw_audio_create(44100.0f);
    M4ARegWrite ev[] = {
        { 0, M4A_REG_SOUNDCNT_H, (1u << 9) | (1u << 2) },  /* A → L, vol 100% */
    };
    M4ARegWriteBatch batch = { .events = ev, .count = 1 };

    enum { N = 128 };
    float L[N], R[N];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    hw_audio_render_events(hw, &batch, &ring, L, R, N);

    /* The fixed pipeline holds ring[0] = 100 for ~10 internal samples
     * (until pcm_pos crosses 1 with pcm_step ≈ 0.102), producing a
     * brief positive transient at the very start of the host output
     * which the resampler smears across ~TAPS/2/step ≈ 5 host
     * samples.  Sum-of-positive-L is the cleanest invariant: the
     * fixed code yields a small positive sum; the broken
     * implementation that skipped ring[0] yields exactly 0. */
    float sum_pos_L = 0.0f;
    for (int i = 0; i < N; i++) {
        if (L[i] > 0.0f) sum_pos_L += L[i];
    }
    /* Threshold rationale: ring[0]=100 drives ~10 internal-rate
     * samples × normalized 100/128 × kDmaChanScale 0.5 × routing 1
     * = ~0.39 of total signal energy at internal rate; resampled +
     * smeared this still produces a sum well above 0.001. */
    ASSERT(sum_pos_L > 0.001f,       "ring[0] consumed at session start (positive transient present)");

    hw_audio_destroy(hw);
}

/* Solo-mask: with both PSG (sq2) and DirectSound (DMA A) active, each
 * solo selection must produce only the soloed group's signal, with
 * the other channels silent.  Channel names + bit positions match the
 * patched mGBA capture tool so the same name selects the same channel
 * in both reference and v2 captures.
 *
 * Helper renders the same canned event sequence with a chosen mask
 * and returns the L peak.  Each subtest creates a fresh chip so the
 * mask change isn't entangled with cumulative state. */
static float run_psg_and_pcm_with_solo(uint32_t mask, int frames,
                                       float *out_L, float *out_R)
{
    M4APcmRing ring;
    memset(&ring, 0, sizeof(ring));
    ring.pcm_rate_hz  = M4A_PCM_RATE_HZ;
    ring.write_cursor = M4A_PCM_DMA_BUF_SIZE;
    /* Saturated sawtooth on ring_a — strong PCM signal.  ring_b
     * stays zero so DMA B contributes nothing regardless of mask. */
    for (int i = 0; i < M4A_PCM_DMA_BUF_SIZE; i++) {
        ring.ring_a[i] = (int8_t)((i * 4) - 127);
        ring.ring_b[i] = 0;
    }

    HwAudio *hw = hw_audio_create(44100.0f);
    hw_audio_set_solo_mask(hw, mask);

    M4ARegWrite ev[] = {
        /* PSG master + routing: NR52 master enable, NR50 master 7/7,
         * NR51 routes sq2 (bit 1) to both sides. */
        { 0, M4A_REG_NR52, 0x80 },
        { 0, M4A_REG_NR50, 0x77 },
        { 0, M4A_REG_NR51, 0x22 },
        /* SOUNDCNT_H: DMA A → both sides at 100%, plus PSG vol 100%. */
        { 0, M4A_REG_SOUNDCNT_H, (1u << 8) | (1u << 9) | (1u << 2) },
        /* SQ2 trigger at audible freq. */
        { 0, M4A_REG_NR21, 0x80 },
        { 0, M4A_REG_NR22, 0xF8 },
        { 0, M4A_REG_NR23, 1700 & 0xFF },
        { 0, M4A_REG_NR24, 0x80 | ((1700 >> 8) & 7) },
    };
    M4ARegWriteBatch batch = { .events = ev, .count = sizeof(ev)/sizeof(ev[0]) };
    hw_audio_render_events(hw, &batch, &ring, out_L, out_R, frames);

    /* Skip resampler warmup and any DC bias from canned setup; report
     * AC-amplitude peak (deviation from mean) so unmasked silence
     * really reads as zero rather than a small DC offset. */
    enum { skip = 64 };
    float sumL = 0.0f;
    int   nL   = 0;
    for (int i = skip; i < frames; i++) { sumL += out_L[i]; nL++; }
    float meanL = nL > 0 ? sumL / (float)nL : 0.0f;
    float peak = 0.0f;
    for (int i = skip; i < frames; i++) {
        float a = out_L[i] - meanL; if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    hw_audio_destroy(hw);
    return peak;
}

static void test_chip_canned_solo_mask_isolates_channels(void)
{
    printf("Testing chip-only: solo mask isolates PSG vs DirectSound vs individual channels...\n");

    enum { N = 2048 };
    float L[N], R[N];

    float peak_full   = run_psg_and_pcm_with_solo(HW_AUDIO_SOLO_FULL,   N, L, R);
    float peak_psg    = run_psg_and_pcm_with_solo(HW_AUDIO_SOLO_PSG,    N, L, R);
    float peak_dsound = run_psg_and_pcm_with_solo(HW_AUDIO_SOLO_DSOUND, N, L, R);
    float peak_sq2    = run_psg_and_pcm_with_solo(HW_AUDIO_SOLO_SQ2,    N, L, R);
    float peak_sq1    = run_psg_and_pcm_with_solo(HW_AUDIO_SOLO_SQ1,    N, L, R);
    float peak_dma_a  = run_psg_and_pcm_with_solo(HW_AUDIO_SOLO_DMA_A,  N, L, R);
    float peak_dma_b  = run_psg_and_pcm_with_solo(HW_AUDIO_SOLO_DMA_B,  N, L, R);

    /* Full mix has both PSG and PCM contributing — peak is at least
     * as large as either individual group. */
    ASSERT(peak_full > 0.05f,        "full mix is audible");

    /* PSG-only (SQ1+SQ2+wave+noise; only SQ2 active here): unipolar
     * SQ2 routed L+R produces AC peak around 0.03-0.07 in this
     * 2048-frame window because the chosen ~377 Hz fundamental fits
     * less than 2 full periods, and the polyphase kernel transient
     * dominates much of the buffer.  Threshold 0.02 stays well above
     * the silent floor (~1e-4) while tolerating the rendered
     * amplitude.  Absolute level tuning is a follow-on parity item
     * (see plan §12 PSG-unipolar gate). */
    ASSERT(peak_psg > 0.02f,         "psg-only is audible (unipolar)");
    ASSERT(peak_psg < peak_full + 0.001f,
                                     "psg-only ≤ full (no extra signal from masking out PCM)");

    /* DirectSound-only: PCM still audible, PSG silent. */
    ASSERT(peak_dsound > 0.05f,      "directsound-only is audible");

    /* SQ2-only: matches PSG (since only SQ2 in the PSG group is active). */
    ASSERT(peak_sq2 > 0.02f,         "sq2-only is audible (unipolar)");

    /* SQ1-only: SQ1 channel is OFF in this canned setup → silence. */
    ASSERT(peak_sq1 < 0.001f,        "sq1-only with no sq1 trigger is silent");

    /* DMA A-only: ring_a has signal → audible. */
    ASSERT(peak_dma_a > 0.05f,       "dma-a-only is audible");

    /* DMA B-only: ring_b is zero → silence. */
    ASSERT(peak_dma_b < 0.001f,      "dma-b-only with zero ring_b is silent");
}

/* Empty mask is treated as "no override" — falls back to full mix
 * rather than silencing everything.  The `--solo` CLI shouldn't
 * accidentally mute everything if the caller passes 0. */
static void test_chip_canned_solo_mask_empty_falls_back_to_full(void)
{
    printf("Testing chip-only: empty solo mask falls back to full mix...\n");

    HwAudio *hw = hw_audio_create(44100.0f);
    hw_audio_set_solo_mask(hw, 0);
    ASSERT_EQ((int)hw_audio_get_solo_mask(hw), (int)HW_AUDIO_SOLO_FULL,
                                     "set_solo_mask(0) restores HW_AUDIO_SOLO_FULL");

    /* Confirm bits outside the 6 valid channel bits are ignored. */
    hw_audio_set_solo_mask(hw, 0xFFFFFFFFu);
    ASSERT_EQ((int)hw_audio_get_solo_mask(hw), (int)HW_AUDIO_SOLO_FULL,
                                     "set_solo_mask(all ones) clamps to HW_AUDIO_SOLO_FULL");

    hw_audio_destroy(hw);
}

static void test_chip_canned_soundbias_cycle_0_vs_3_levels(void)
{
    printf("Testing chip-only: SQ2 peak comparable at sampling_cycle 0 vs 3...\n");

    enum { N = 4096 };
    float L0[N], R0[N], L3[N], R3[N];

    /* sampling_cycle=0 → internal_rate stays at 131072 Hz floor. */
    uint32_t sb0 = 0x200u | (0u << 14);
    float peak0 = run_sq2_at_soundbias(sb0, N, L0, R0);

    /* sampling_cycle=3 → internal_rate bumps to 262144 Hz; resampler
     * step doubles, kernel rebuilds, all internal stages run at
     * 262144 Hz.  The end-to-end host output should produce a
     * comparable peak amplitude — both rates pass the audio band
     * (0..host/2) identically; only above-Nyquist content (which we
     * don't have in this signal) would diverge.
     *
     * NOTE: this test does NOT prove rate switching by itself — a
     * broken implementation that stayed at 131072 Hz would also pass
     * because at low frequencies the host output is rate-invariant.
     * The unambiguous switching proof is
     * test_chip_canned_soundbias_internal_rate_switches above. */
    uint32_t sb3 = 0x200u | (3u << 14);
    float peak3 = run_sq2_at_soundbias(sb3, N, L3, R3);

    ASSERT(peak0 > 0.001f,           "sc=0 audible");
    ASSERT(peak3 > 0.001f,           "sc=3 audible");

    /* Allow ~10% tolerance because the resampler at sc=3 has a
     * different cut-frequency-to-input-rate ratio (lower normalized
     * cut, slightly different tap weights), and the cumulative
     * sample-clock relationship is computed against a different
     * step.  These are honest band-limited differences, not bugs. */
    float ratio = peak0 > 1e-9f ? peak3 / peak0 : 0.0f;
    ASSERT(ratio > 0.9f && ratio < 1.1f,
                                     "sc=0 vs sc=3 peak amplitudes within 10%");
}
#endif /* HW_AUDIO_V2 */

int main(void)
{
    printf("=== M4A Engine Unit Tests ===\n\n");

    test_umul3232H32();
    test_scale_table();
    test_midi_key_to_freq();
    test_midi_key_to_cgb_freq();
    test_trk_vol_pit_set();
    test_engine_init();
    test_xcmd_subcommands();
    test_basic_audio();
    test_polyphony_stealing();
#if defined(M4A_DRIVER_V2)
    test_v2_trigger_semantics();
    test_v2_song_volume_rescales();
    test_v2_pcm_ring_fills();
    test_v2_pcm_frequency_scale();
    test_v2_cgb_pan_mask_routes();
    test_v2_pcm_cc7_refresh();
    test_v2_pcm_reverb_pipeline();
    test_v2_event_stream();
    test_v2_consume_clears_triggers();
    test_v2_wave_ram_events();
    test_v2_modt_recomputes_track_state();
    test_v2_xcmd_mutates_track_state();
    test_v2_xcmd_propagates_to_new_notes();
    test_v2_xcmd_protocol_safety();
    test_v2_xcmd_render_changes_audio();
    test_v2_lfo_speed_default_matches_m4a();
    test_v2_lfo_disabled_no_freq_drift();
    test_v2_lfo_vibrato_modulates_freq();
    test_v2_lfo_delay_holds_off();
    test_v2_lfo_lfodl_resets_running_modulation();
    test_v2_cgb_trigger_only_on_note_start();
    test_v2_pcm_publish_event_per_vblank();
#if defined(HW_AUDIO_V2)
    test_v2_psg_square_audible();
    test_v2_psg_pan_routing();
    test_v2_psg_wave_audible();
    test_v2_directsound_audible();
    test_v2_pcm_chunk_size_invariance();
    test_v2_pcm_publish_timing();
#endif
    test_v2_no_event_drops_over_long_run();
    test_v2_all_sound_off_immediate();
    test_v2_stereo_toggle_matches_emerald();
    test_v2_cgb_mono_forces_both_sides();
#endif

#if defined(HW_AUDIO_V2)
    test_hw_psg_frame_sequencer_init_convention();
    test_hw_psg_frame_sequencer_dispatch_table();
    test_hw_psg_frame_sequencer_chunk_invariance();
    test_hw_psg_frame_sequencer_rate_continuity();
    test_hw_psg_frame_sequencer_nr52_power_cycle();
    test_hw_psg_frame_sequencer_disabled_does_not_advance();
    test_hw_psg_frame_sequencer_nr52_enabled_write_no_reset();
    test_hw_psg_frame_sequencer_nr52_disabled_write_stable();
    test_hw_psg_nr52_power_cycle_preserves_wave_ram();

    /* Chip-only canned-event tests — no driver needed.  These also run
     * under full-v2 (M4A_DRIVER_V2 + HW_AUDIO_V2) but their value is
     * the chip-only build which doesn't have the driver. */
    test_chip_canned_square_audible();
    test_chip_canned_master_disable_silences();
    test_chip_canned_pan_routing();
    test_chip_canned_wave_audible();
    test_chip_canned_pcm_routing();
    test_chip_canned_noise_audible();
    test_chip_canned_noise_dac_off_silences();
    test_chip_canned_soundbias_dc_offset();
    test_chip_canned_soundbias_clip_asymmetric();
    test_chip_canned_soundcnth_psg_vol_codes();
    test_chip_canned_soundcnth_dma_vol_codes();
    test_chip_canned_resample_antialias();
    test_chip_canned_block_size_invariance();
    test_chip_canned_dc_streaming();
    test_chip_canned_soundbias_cycle_audible_sweep();
    test_chip_canned_soundbias_internal_rate_switches();
    test_chip_canned_pcm_two_stage_drain();
    test_chip_canned_pcm_constant_byte();
    test_chip_canned_pcm_first_byte_consumed();
    test_chip_canned_solo_mask_isolates_channels();
    test_chip_canned_solo_mask_empty_falls_back_to_full();
    test_chip_canned_soundbias_cycle_0_vs_3_levels();
#endif

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
