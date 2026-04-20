#ifndef M4A_PLUGIN_H
#define M4A_PLUGIN_H

#include <stdatomic.h>
#include "m4a_engine.h"
#include "voicegroup/voicegroup_loader.h"
#include "voicegroup/vg_available.h"
#include "voicegroup/project_asset_index.h"
#include "m4a_gui.h"
#include <clap/clap.h>

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
    uint8_t maxPcmChannels;
    bool activated;
    /* Incremented from the audio thread when incoming MIDI/note activity is
     * seen. The GUI polls it from the main thread and handles the visual decay. */
    atomic_uint midiActivitySeq;
    atomic_uint xcmdActivitySeq;
    atomic_uint pendingXcmdSeq;
    atomic_uint pendingXcmdMeta;
    atomic_uint latestXcmdSeq;
    atomic_uint latestXcmdMeta;
    atomic_uint latestXcmdValue;
    /* "Add instrument" control channel. ccomidi sends a CC#98 (index LSB)
     * immediately followed by a CC#99 (index MSB + trigger) every time the
     * user picks an instrument. pendingAddIndexLsb holds the last LSB
     * received; on CC#99 the audio thread composes the full 14-bit index
     * into pendingAddIndex and bumps pendingAddSeq. The GUI thread observes
     * the seq change, appends availableInstruments[index].macro to the
     * voicegroup file, and triggers a reload. */
    atomic_uint pendingAddIndexLsb;
    atomic_uint pendingAddIndex;
    atomic_uint pendingAddSeq;
    /* CLAP param mirror for per-track program selection.
     * Kept outside the engine so params/state can read it without poking
     * directly at audio-thread-owned track state. */
    atomic_uchar programParams[MAX_TRACKS];

    /* Voice editor: snapshot of original voices and per-voice override flags */
    ToneData originalVoices[VOICEGROUP_SIZE];
    bool voiceOverrides[VOICEGROUP_SIZE];

    /* Project-wide sample catalog and per-voice sample overrides */
    ProjectAssetIndex *assetIndex;

    /* Project-wide "available to append" instrument list. Rebuilt whenever
     * the voicegroup is (re)loaded. ccomidi picks a display name from
     * state.json; on CC#99 the plugin appends availableInstruments[index].macro
     * to data->loadedVg->sourceFile. */
    AvailableInstrumentList availableInstruments;

    /* GUI */
    const clap_host_t *host;
    M4AGuiState *gui;
    clap_id guiTimerId;
    unsigned int guiMidiActivitySeqSeen;
    unsigned int guiXcmdActivitySeqSeen;
    unsigned int guiPendingXcmdSeqSeen;
    unsigned int guiLatestXcmdSeqSeen;
    unsigned int guiPendingAddSeqSeen;

    /* Set when the plugin calls request_restart (e.g. after Reload).
     * The standalone polls this to perform the actual restart cycle. */
    bool restartRequested;
} M4APluginData;

#endif /* M4A_PLUGIN_H */
