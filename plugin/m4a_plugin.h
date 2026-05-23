#ifndef M4A_PLUGIN_H
#define M4A_PLUGIN_H

#include <stdatomic.h>
#include "m4a_engine.h"
#include "voicegroup/voicegroup_loader.h"
#include "voicegroup/project_asset_index.h"
#include "m4a_gui.h"
#include <clap/clap.h>

#if defined(M4A_DRIVER_V2)
#include "m4a/m4a_driver.h"
#endif
#if defined(HW_AUDIO_V2)
#include "hw_audio/hw_audio.h"
#endif

typedef struct {
    M4AEngine engine;
    LoadedVoiceGroup *loadedVg;
    VoicegroupLoaderConfig loaderConfig;
    char projectRoot[512];
    char voicegroupName[256];
    uint8_t reverbAmount;
    uint8_t masterVolume; // The m4a-level master volume (0-15)
    uint8_t songMasterVolume; // The song-level master volume (0-127)
    bool analogFilter;
    bool monoOutput;
    uint8_t maxPcmChannels;
    bool activated;
    /* Per-channel activity counters. Incremented from the audio thread at the
     * channel's index whenever a MIDI/note event arrives for that channel;
     * the GUI polls them from the main thread and pulses the matching LED. */
    atomic_uint midiActivitySeq[MAX_TRACKS];
    atomic_uint xcmdActivitySeq;
    atomic_uint pendingXcmdSeq;
    atomic_uint pendingXcmdMeta;
    atomic_uint latestXcmdSeq;
    atomic_uint latestXcmdMeta;
    atomic_uint latestXcmdValue;
    /* CLAP param mirror for per-track program selection.
     * Kept outside the engine so params/state can read it without poking
     * directly at audio-thread-owned track state. */
    atomic_uchar programParams[MAX_TRACKS];

    /* Voice editor: snapshot of original voices and per-voice override flags */
    ToneData originalVoices[VOICEGROUP_SIZE];
    bool voiceOverrides[VOICEGROUP_SIZE];

    /* Project-wide sample catalog and per-voice sample overrides */
    ProjectAssetIndex *assetIndex;

    /* GUI */
    const clap_host_t *host;
    M4AGuiState *gui;
    clap_id guiTimerId;
    unsigned int guiMidiActivitySeqSeen[MAX_TRACKS];
    unsigned int guiXcmdActivitySeqSeen;
    unsigned int guiPendingXcmdSeqSeen;
    unsigned int guiLatestXcmdSeqSeen;

    /* Set when the plugin calls request_restart (e.g. after Reload).
     * The standalone polls this to perform the actual restart cycle. */
    bool restartRequested;

    /* Recorder UI/wire state. The RecorderCore lives on the engine.
     * `recorderArmed` gates the audio thread's push calls.
     * `recorderPath` is the last-typed filename (persisted via CLAP state).
     * `recorderSeen*` are bitmasks (bit n = channel n) latched when the
     * recorder captures a PC / volume CC / pan CC for that channel; cleared
     * by the Clear button so they track the current buffer contents. */
    atomic_bool recorderArmed;
    char        recorderPath[512];
    atomic_uint recorderSeenPC;
    atomic_uint recorderSeenVol;
    atomic_uint recorderSeenPan;

    /* Standalone-only: preferred audio output device name, read from
     * poryaaaa.cfg's `audio_output=` key. Empty string = use system default.
     * Consumed by the macOS standalone entry before startAudioThread runs. */
    char        audioOutputName[128];

    /* External MIDI clock sync (standalone or any host without transport).
     * Driven by 0xF8 (24 PPQ), 0xFA/FB/FC start/continue/stop, 0xF2 SPP. */
    uint64_t extClockSampleCounter;   /* running sample-time, +=frames each block */
    uint64_t extClockLastSampleTime;  /* sample-time of last 0xF8 */
    double   extClockBpm;             /* smoothed BPM derived from clock interval */
    bool     extClockInitialized;     /* set on first 0xF8 after reset/Start */
    bool     extClockPlaying;         /* set by 0xFA/FB, cleared by 0xFC */

#if defined(M4A_DRIVER_V2)
    M4ADriver *m4a_v2;
#endif
#if defined(HW_AUDIO_V2)
    HwAudio *hw_v2;
#endif
} M4APluginData;

#endif /* M4A_PLUGIN_H */
