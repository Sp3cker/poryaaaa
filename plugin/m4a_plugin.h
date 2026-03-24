#ifndef M4A_PLUGIN_H
#define M4A_PLUGIN_H

#include "m4a_engine.h"
#include "voicegroup_loader.h"
#include "m4a_gui.h"
#include <clap/clap.h>
#include <stdatomic.h>

/* Automation parameter IDs exposed to the host via CLAP_EXT_PARAMS.
 * These map to M4A engine CC values on the plugin's single track (track 0). */
enum {
    PARAM_VOLUME     = 0,   /* CC#7  - Track volume (0-127) */
    PARAM_PAN        = 1,   /* CC#10 - Pan (0-127, 64=center) */
    PARAM_MOD_DEPTH  = 2,   /* CC#1  - Modulation / vibrato depth (0-127) */
    PARAM_BEND_RANGE = 3,   /* CC#20 - Pitch bend range in semitones (1-24) */
    PARAM_LFO_SPEED  = 4,   /* CC#21 - LFO speed (0-127) */
    PARAM_REVERB     = 5,   /* Global reverb amount (0-127) */
    PARAM_COUNT      = 6,
};

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

    /* Shadow parameter values and pending host notifications for stepped
     * automation parameters. All values are stored as raw integer steps. */
    atomic_uchar paramValues[PARAM_COUNT];
    atomic_uchar pendingParamValues[PARAM_COUNT];
    atomic_uint pendingParamEventFlags[PARAM_COUNT];
    atomic_bool pendingParamOutputs[PARAM_COUNT];
    atomic_bool pendingParamGestures[PARAM_COUNT];

    /* Voice editor: snapshot of original voices and per-voice override flags */
    ToneData originalVoices[VOICEGROUP_SIZE];
    bool voiceOverrides[VOICEGROUP_SIZE];

    /* GUI */
    const clap_host_t *host;
    M4AGuiState *gui;
    clap_id guiTimerId;

    /* Set when the plugin calls request_restart (e.g. after Reload).
     * The standalone polls this to perform the actual restart cycle. */
    bool restartRequested;
} M4APluginData;

#endif /* M4A_PLUGIN_H */
