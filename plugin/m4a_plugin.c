#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/draft/undo.h>
#include "m4a_plugin.h"
#include "m4a_params.h"
#include "m4a_engine.h"
#include "m4a_engine_recorder.h"
#include "m4a_channel.h"
#include "m4a_reverb.h"
#include "voicegroup/voicegroup_loader.h"
#include "voicegroup/voicegroup_state.h"
#include "m4a_gui.h"

/*
 * M4A VSTi Plugin - CLAP implementation
 *
 * A CLAP instrument plugin that uses the GBA m4a sound engine to render audio.
 * Receives MIDI input from the DAW and produces stereo audio output.
 */

/* Plugin descriptor */
static const char *s_features[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_SAMPLER,
    CLAP_PLUGIN_FEATURE_STEREO,
    NULL
};

static const clap_plugin_descriptor_t s_descriptor = {
    .clap_version = CLAP_VERSION,
    .id = "com.huderlem.poryaaaa",
    .name = "poryaaaa",
    .vendor = "pokeemerald",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "GBA M4A sound engine plugin for pokeemerald music preview",
    .features = s_features,
};

/* ---- Config file ---- */

/*
 * Directory of the loaded .clap file, set during entry_init.
 * Used to find poryaaaa.cfg in the same directory as the plugin.
 */
static char s_pluginDir[512] = {0};

/* Optional diagnostic log path, set from config key "log=<path>" */
static const char *s_pluginLogPath = NULL;

static void plugin_engine_xcmd(void *ctx, int trackIndex, uint8_t selector, uint32_t value);
static bool plugin_poll_pending_xcmd(M4APluginData *data, unsigned int *seq_out,
                                     unsigned int *meta_out);
static bool plugin_poll_latest_xcmd(M4APluginData *data, unsigned int *seq_out,
                                    unsigned int *meta_out, unsigned int *value_out);
static void plugin_format_pending_xcmd(char *buf, size_t buf_size, unsigned int meta);

/*
 * Load settings from poryaaaa.cfg placed next to the .clap file.
 *
 * The config file uses simple key=value lines, one per line.
 * Lines starting with '#' are comments and are ignored.
 *
 * Supported keys:
 *   project_root   - Path to the pokeemerald project directory
 *   voicegroup     - Voicegroup name (e.g. petalburg, littleroot_town)
 *   reverb         - Reverb amount (0-127)
 *   master_volume  - Master volume (0-15)
 *   analog_filter  - GBA analog output low-pass filter (0=off, 1=on)
 *   mono_output    - Route output like Emerald mono mode (0=stereo, 1=mono)
 */
static void load_config_file(M4APluginData *data)
{
    if (s_pluginDir[0] == '\0')
        return;

    char configPath[600];
    snprintf(configPath, sizeof(configPath), "%s/poryaaaa.cfg", s_pluginDir);

    FILE *f = fopen(configPath, "r");
    if (!f)
        return;

    char line[600];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline and carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = line;
        const char *value = eq + 1;

        if (strcmp(key, "log") == 0) {
            s_pluginLogPath = strdup(value); /* leak is fine for a dev diagnostic */
        } else if (strcmp(key, "project_root") == 0) {
            snprintf(data->projectRoot, sizeof(data->projectRoot), "%s", value);
        } else if (strcmp(key, "voicegroup") == 0) {
            snprintf(data->voicegroupName, sizeof(data->voicegroupName), "%s", value);
        } else if (strcmp(key, "reverb") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            data->reverbAmount = (uint8_t)v;
        } else if (strcmp(key, "master_volume") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 15) v = 15;
            data->masterVolume = (uint8_t)v;
        } else if (strcmp(key, "song_master_volume") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > MAX_SONG_VOLUME) v = MAX_SONG_VOLUME;
            data->songMasterVolume = (uint8_t)v;
        } else if (strcmp(key, "analog_filter") == 0) {
            data->analogFilter = (atoi(value) != 0);
        } else if (strcmp(key, "mono_output") == 0) {
            data->monoOutput = (atoi(value) != 0);
        } else if (strcmp(key, "max_channels") == 0) {
            int v = atoi(value);
            if (v < 1) v = 1;
            if (v > MAX_PCM_CHANNELS) v = MAX_PCM_CHANNELS;
            data->maxPcmChannels = (uint8_t)v;
        } else if (strcmp(key, "audio_output") == 0) {
            /* Standalone audio device name (e.g. "BlackHole 2ch"). Substring
             * matched against RtAudio's device enumeration at startup. */
            while (*value == ' ') value++;
            snprintf(data->audioOutputName, sizeof(data->audioOutputName),
                     "%s", value);
        } else if (strcmp(key, "sound_data_paths") == 0) {
            /* Semicolon-separated list of extra .inc files, relative to project_root */
            char tmp[600];
            strncpy(tmp, value, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *tok = strtok(tmp, ";");
            while (tok && data->loaderConfig.soundDataPathCount < 8) {
                while (*tok == ' ') tok++;
                int idx = data->loaderConfig.soundDataPathCount++;
                snprintf(data->loaderConfig.soundDataPaths[idx],
                         sizeof(data->loaderConfig.soundDataPaths[idx]), "%s", tok);
                tok = strtok(NULL, ";");
            }
        } else if (strcmp(key, "voicegroup_paths") == 0) {
            char tmp[600];
            strncpy(tmp, value, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *tok = strtok(tmp, ";");
            while (tok && data->loaderConfig.voicegroupPathCount < 8) {
                while (*tok == ' ') tok++;
                int idx = data->loaderConfig.voicegroupPathCount++;
                snprintf(data->loaderConfig.voicegroupPaths[idx],
                         sizeof(data->loaderConfig.voicegroupPaths[idx]), "%s", tok);
                tok = strtok(NULL, ";");
            }
        }
    }

    fclose(f);
}

/* ---- Plugin lifecycle ---- */

static bool plugin_init(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    data->masterVolume = 15;
    data->songMasterVolume = MAX_SONG_VOLUME;
    data->reverbAmount = 0;
    data->analogFilter = false;
    data->monoOutput = false;
    data->maxPcmChannels = 5;
    data->projectRoot[0] = '\0';
    data->voicegroupName[0] = '\0';
    data->loadedVg = NULL;
    data->activated = false;
    data->gui = NULL;
    data->guiTimerId = CLAP_INVALID_ID;
    for (int i = 0; i < MAX_TRACKS; i++) {
        atomic_init(&data->midiActivitySeq[i], 0);
        data->guiMidiActivitySeqSeen[i] = 0;
    }
    atomic_init(&data->xcmdActivitySeq, 0);
    atomic_init(&data->pendingXcmdSeq, 0);
    atomic_init(&data->pendingXcmdMeta, 0);
    atomic_init(&data->latestXcmdSeq, 0);
    atomic_init(&data->latestXcmdMeta, 0);
    atomic_init(&data->latestXcmdValue, 0);
    data->guiXcmdActivitySeqSeen = 0;
    data->guiPendingXcmdSeqSeen = 0;
    data->guiLatestXcmdSeqSeen = 0;
    data->assetIndex = NULL;
    atomic_init(&data->recorderArmed, false);
    data->recorderPath[0] = '\0';
    atomic_init(&data->recorderSeenPC,  0u);
    atomic_init(&data->recorderSeenVol, 0u);
    atomic_init(&data->recorderSeenPan, 0u);
    data->audioOutputName[0] = '\0';
    data->extClockSampleCounter = 0;
    data->extClockLastSampleTime = 0;
    data->extClockBpm = 0.0;
    data->extClockInitialized = false;
    data->extClockPlaying = false;
    m4a_params_init(data);
    /* Load defaults from config file placed next to the .clap */
    load_config_file(data);
    /* Forward the log path into the voicegroup loader so it can emit diagnostics */
    voicegroup_loader_set_log_path(s_pluginLogPath);

    /* Build the project asset index once at init (used by sample swapper) */
    if (data->projectRoot[0]) {
        data->assetIndex = project_asset_index_create();
        if (data->assetIndex)
            project_asset_index_rebuild(data->assetIndex, data->projectRoot, &data->loaderConfig);
    }
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    /* GUI must already be destroyed by the host (gui->destroy before plugin->destroy) */
    if (data->loadedVg) {
        voicegroup_free(data->loadedVg);
        data->loadedVg = NULL;
    }
    project_asset_index_destroy(data->assetIndex);
    data->assetIndex = NULL;
    m4a_engine_destroy(&data->engine);
#if defined(M4A_DRIVER_V2)
    if (data->m4a_v2) { m4a_driver_destroy(data->m4a_v2); data->m4a_v2 = NULL; }
#endif
#if defined(HW_AUDIO_V2)
    if (data->hw_v2)  { hw_audio_destroy(data->hw_v2);    data->hw_v2  = NULL; }
#endif
    free(data);
    free((void *)plugin);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate,
                            uint32_t min_frames, uint32_t max_frames)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_init(&data->engine, (float)sample_rate);
    m4a_engine_recorder_set_sample_rate(&data->engine, sample_rate);
#if defined(M4A_DRIVER_V2)
    data->m4a_v2 = m4a_driver_create((float)sample_rate);
#endif
#if defined(HW_AUDIO_V2)
    data->hw_v2 = hw_audio_create((float)sample_rate);
#endif
    m4a_engine_set_xcmd_callback(&data->engine, plugin_engine_xcmd, data);
#if defined(M4A_DRIVER_V2)
    m4a_driver_set_xcmd_callback(data->m4a_v2,
        (M4ADriverXcmdFn)plugin_engine_xcmd, data);
#endif
    data->engine.masterVolume = data->masterVolume;
    data->engine.songMasterVolume = data->songMasterVolume;
    data->engine.analogFilter = data->analogFilter;
    data->engine.maxPcmChannels = data->maxPcmChannels;
    m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);
#if defined(M4A_DRIVER_V2)
    m4a_set_master_volume(data->m4a_v2, data->masterVolume);
    m4a_set_song_volume(data->m4a_v2, data->songMasterVolume);
    m4a_set_analog_filter(data->m4a_v2, data->analogFilter);
    m4a_set_stereo(data->m4a_v2, !data->monoOutput);
    m4a_set_max_pcm_channels(data->m4a_v2, data->maxPcmChannels);
    m4a_set_reverb_amount(data->m4a_v2, data->reverbAmount);
#endif

    /* Build asset index if not yet created (e.g. projectRoot set via state_load) */
    if (data->projectRoot[0] && !data->assetIndex) {
        data->assetIndex = project_asset_index_create();
        if (data->assetIndex)
            project_asset_index_rebuild(data->assetIndex, data->projectRoot, &data->loaderConfig);
    }

    /* If voicegroup is configured, load it */
    if (data->projectRoot[0] && data->voicegroupName[0]) {
        if (data->loadedVg) {
            voicegroup_free(data->loadedVg);
            data->loadedVg = NULL;
        }
        data->loadedVg = voicegroup_load(data->projectRoot, data->voicegroupName,
                                         &data->loaderConfig);
        if (data->loadedVg) {
            m4a_engine_set_voicegroup(&data->engine, data->loadedVg->voices);
#if defined(M4A_DRIVER_V2)
            m4a_driver_set_voicegroup(data->m4a_v2, data->loadedVg->voices);
#endif
            memcpy(data->originalVoices, data->loadedVg->voices, sizeof(data->originalVoices));
            memset(data->voiceOverrides, 0, sizeof(data->voiceOverrides));
            /* Apply any pending sample overrides */
            if (data->assetIndex)
                project_asset_index_apply_overrides(data->assetIndex, data->projectRoot, data->loadedVg);
            m4a_params_sync_to_engine(data);
            voicegroup_state_write_default(data->projectRoot,
                                           data->voicegroupName,
                                           data->loadedVg);
        }
    }

    data->activated = true;

    /* Update voice data pointers for the GUI */
    if (data->gui) {
        if (data->loadedVg)
            m4a_gui_set_voice_data(data->gui, data->loadedVg->voices, data->originalVoices, data->voiceOverrides);
        else
            m4a_gui_set_voice_data(data->gui, NULL, NULL, NULL);

        /* Provide project asset catalog to the GUI for the sample selector */
        if (data->assetIndex)
            m4a_gui_set_project_assets(data->gui,
                                       data->assetIndex->directsoundAssets, data->assetIndex->directsoundCount,
                                       data->assetIndex->progWaveAssets, data->assetIndex->progWaveCount,
                                       data->assetIndex->overrides);
        else
            m4a_gui_set_project_assets(data->gui, NULL, 0, NULL, 0, NULL);
    }

    /* Notify GUI of current voicegroup status */
    if (data->gui) {
        M4AGuiSettings gs;
        memset(&gs, 0, sizeof(gs));
        snprintf(gs.projectRoot,    sizeof(gs.projectRoot),    "%s", data->projectRoot);
        snprintf(gs.voicegroupName, sizeof(gs.voicegroupName), "%s", data->voicegroupName);
        gs.reverbAmount      = data->reverbAmount;
        gs.masterVolume      = data->masterVolume;
        gs.songMasterVolume  = data->songMasterVolume;
        gs.analogFilter      = data->analogFilter;
        gs.maxPcmChannels    = data->maxPcmChannels;
        gs.voicegroupLoaded  = (data->loadedVg != NULL);
        m4a_gui_update_settings(data->gui, &gs);
    }

    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (data->gui)
        m4a_gui_set_voice_data(data->gui, NULL, NULL, NULL);
    m4a_engine_destroy(&data->engine);
#if defined(M4A_DRIVER_V2)
    if (data->m4a_v2) { m4a_driver_destroy(data->m4a_v2); data->m4a_v2 = NULL; }
#endif
#if defined(HW_AUDIO_V2)
    if (data->hw_v2)  { hw_audio_destroy(data->hw_v2);    data->hw_v2  = NULL; }
#endif
    data->activated = false;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_all_sound_off(&data->engine);
#if defined(M4A_DRIVER_V2)
    m4a_all_sound_off(data->m4a_v2);
#endif
    m4a_reverb_reset(&data->engine.reverb);
    m4a_engine_lpf_reset(&data->engine);
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_all_sound_off(&data->engine);
#if defined(M4A_DRIVER_V2)
    m4a_all_sound_off(data->m4a_v2);
#endif
    m4a_reverb_reset(&data->engine.reverb);
    m4a_engine_lpf_reset(&data->engine);
}

/* ---- MIDI event processing ---- */

/* External MIDI clock pulse: derive BPM from interval between consecutive 0xF8s
 * (24 pulses per quarter), apply a light EMA so jitter doesn't shake the engine,
 * and push the result into the same APIs the host-transport path uses. */
static void process_midi_clock_pulse(M4APluginData *data, uint32_t sample_in_block)
{
    uint64_t now = data->extClockSampleCounter + sample_in_block;

    if (!data->extClockInitialized) {
        data->extClockInitialized = true;
        data->extClockLastSampleTime = now;
        /* First-pulse log so users can confirm pass-through without a DAW. */
        fprintf(stderr, "[poryaaaa] external MIDI clock detected\n");
        return;
    }

    uint64_t dt = now - data->extClockLastSampleTime;
    data->extClockLastSampleTime = now;
    if (dt == 0) return;

    double sr = (double)data->engine.sampleRate;
    if (sr <= 0.0) return;

    /* 24 PPQ: one quarter-note = 24 clocks; bpm = 60 * sr / (samples_per_quarter). */
    double instBpm = 60.0 * sr / ((double)dt * 24.0);
    if (instBpm < 20.0 || instBpm > 999.0) return;  /* reject obvious jitter spikes */

    if (data->extClockBpm <= 0.0)
        data->extClockBpm = instBpm;
    else
        data->extClockBpm = data->extClockBpm * 0.85 + instBpm * 0.15;

    m4a_engine_set_tempo_bpm(&data->engine, data->extClockBpm);
    m4a_engine_recorder_set_tempo(&data->engine, sample_in_block, data->extClockBpm);
#if defined(M4A_DRIVER_V2)
    m4a_set_tempo_bpm(data->m4a_v2, data->extClockBpm);
#endif
}

/* Status bytes 0xF0..0xFF have no channel nibble. Handle the subset that's
 * useful for transport sync; ignore the rest. */
static void process_midi_system_event(M4APluginData *data, const uint8_t *msg,
                                       uint32_t sample_in_block)
{
    switch (msg[0]) {
    case 0xF2: {  /* Song Position Pointer: data1|data2<<7 = 16th-note count */
        if (data->extClockBpm <= 0.0) break;  /* need a tempo before we can seek */
        uint32_t sixteenths = (uint32_t)msg[1] | ((uint32_t)msg[2] << 7);
        double posSec = ((double)sixteenths * 60.0) / (data->extClockBpm * 4.0);
        m4a_engine_recorder_update_loop(&data->engine, false, 0.0, 0.0, posSec);
        break;
    }
    case 0xF8:  /* MIDI Clock (24 PPQ) */
        process_midi_clock_pulse(data, sample_in_block);
        break;
    case 0xFA:  /* Start */
        data->extClockInitialized = false;
        data->extClockBpm = 0.0;
        data->extClockPlaying = true;
        m4a_engine_recorder_reset(&data->engine);
        break;
    case 0xFB:  /* Continue */
        data->extClockPlaying = true;
        break;
    case 0xFC:  /* Stop */
        data->extClockPlaying = false;
        data->extClockInitialized = false;
        break;
    default:
        break;
    }
}

static void process_midi_event(M4APluginData *data, const uint8_t *msg,
                               uint32_t sample_in_block)
{
    /* System messages (0xF0..0xFF) have no channel nibble — route separately
     * so they don't pulse a phantom channel-LED or get masked as a no-op. */
    if (msg[0] >= 0xF0) {
        process_midi_system_event(data, msg, sample_in_block);
        return;
    }

    uint8_t status = msg[0] & 0xF0;
    uint8_t channel = msg[0] & 0x0F;
    if (channel < MAX_TRACKS)
        atomic_fetch_add(&data->midiActivitySeq[channel], 1);

    switch (status) {
    case 0x90: /* Note On */
        if (msg[2] > 0) {
            m4a_engine_note_on(&data->engine, channel, msg[1], msg[2]);
#if defined(M4A_DRIVER_V2)
            m4a_note_on(data->m4a_v2, channel, msg[1], msg[2]);
#endif
        } else {
            /* velocity 0 = note off */
            m4a_engine_note_off(&data->engine, channel, msg[1]);
#if defined(M4A_DRIVER_V2)
            m4a_note_off(data->m4a_v2, channel, msg[1]);
#endif
        }
        break;
    case 0x80: /* Note Off */
        m4a_engine_note_off(&data->engine, channel, msg[1]);
#if defined(M4A_DRIVER_V2)
        m4a_note_off(data->m4a_v2, channel, msg[1]);
#endif
        break;
    case 0xC0: /* Program Change */
        /* Keep the CLAP param mirror in sync even when the source of truth is
         * an incoming MIDI program-change rather than host automation. */
        m4a_params_set_program(data, channel, msg[1]);
        m4a_engine_program_change(&data->engine, channel, msg[1]);
#if defined(M4A_DRIVER_V2)
        m4a_program_change(data->m4a_v2, channel, msg[1]);
#endif
        break;
    case 0xB0: /* Control Change */
        if (msg[1] == 0x1D || msg[1] == 0x1E || msg[1] == 0x1F)
            atomic_fetch_add(&data->xcmdActivitySeq, 1);
        if (msg[1] == 0x1E) {
            unsigned int meta = ((unsigned int)channel & 0xFFu)
                              | ((unsigned int)msg[2] << 8);

            atomic_store_explicit(&data->pendingXcmdMeta, meta, memory_order_relaxed);
            atomic_fetch_add_explicit(&data->pendingXcmdSeq, 1, memory_order_release);
        }
        m4a_engine_cc(&data->engine, channel, msg[1], msg[2]);
#if defined(M4A_DRIVER_V2)
        m4a_cc(data->m4a_v2, channel, msg[1], msg[2]);
#endif
        break;
    case 0xE0: /* Pitch Bend */
    {
        int16_t bend = ((int16_t)msg[2] << 7 | msg[1]) - 8192;
        m4a_engine_pitch_bend(&data->engine, channel, bend);
#if defined(M4A_DRIVER_V2)
        m4a_pitch_bend(data->m4a_v2, channel, bend);
#endif
        break;
    }
    }

    /* Record MIDI to the embedded recorder when armed. The recorder's
     * samplePosition advances every block regardless of armed (tape-recorder
     * model: time keeps flowing, the toggle only decides whether the current
     * moment gets stamped into the buffer), so the displayed duration always
     * reflects elapsed session time, and disarmed periods leave wall-time
     * gaps in the SMF. samplePosition is monotonic so no reorder is possible.
     * While capturing, latch the recorder-tab per-channel PC/Vol/Pan
     * indicators so the GUI can show what's been captured. */
    if (atomic_load(&data->recorderArmed)) {
        m4a_engine_recorder_push(&data->engine, sample_in_block,
                                 msg[0], msg[1], msg[2]);
        unsigned bit = 1u << channel;
        if (status == 0xC0) atomic_fetch_or(&data->recorderSeenPC, bit);
        else if (status == 0xB0 && msg[1] == 0x07) atomic_fetch_or(&data->recorderSeenVol, bit);
        else if (status == 0xB0 && msg[1] == 0x0A) atomic_fetch_or(&data->recorderSeenPan, bit);
    }
}

static void process_clap_note_event(M4APluginData *data, const clap_event_note_t *ev,
                                    uint32_t sample_in_block)
{
    int channel = ev->channel >= 0 ? ev->channel : 0;
    if (channel >= MAX_TRACKS) channel = 0;
    atomic_fetch_add(&data->midiActivitySeq[channel], 1);

    if (ev->header.type == CLAP_EVENT_NOTE_ON) {
        uint8_t velocity = (uint8_t)(ev->velocity * 127.0 + 0.5);
        if (velocity == 0) velocity = 1;
        m4a_engine_note_on(&data->engine, channel, (uint8_t)ev->key, velocity);
#if defined(M4A_DRIVER_V2)
        m4a_note_on(data->m4a_v2, channel, (uint8_t)ev->key, velocity);
#endif
    } else if (ev->header.type == CLAP_EVENT_NOTE_OFF) {
        m4a_engine_note_off(&data->engine, channel, (uint8_t)ev->key);
#if defined(M4A_DRIVER_V2)
        m4a_note_off(data->m4a_v2, channel, (uint8_t)ev->key);
#endif
    } else if (ev->header.type == CLAP_EVENT_NOTE_CHOKE) {
        m4a_engine_note_off(&data->engine, channel, (uint8_t)ev->key);
#if defined(M4A_DRIVER_V2)
        m4a_note_off(data->m4a_v2, channel, (uint8_t)ev->key);
#endif
    }

    /* Record CLAP note events to the embedded recorder when armed. */
    if (atomic_load(&data->recorderArmed)) {
        uint8_t status, d1, d2;
        if (ev->header.type == CLAP_EVENT_NOTE_ON) {
            status = 0x90 | (channel & 0x0F);
            d1 = (uint8_t)ev->key;
            d2 = (uint8_t)(ev->velocity * 127.0 + 0.5);
            if (d2 == 0) d2 = 1;
        } else {  /* NOTE_OFF or NOTE_CHOKE */
            status = 0x80 | (channel & 0x0F);
            d1 = (uint8_t)ev->key;
            d2 = 0;
        }
        m4a_engine_recorder_push(&data->engine, sample_in_block, status, d1, d2);
    }
}

static void plugin_engine_xcmd(void *ctx, int trackIndex, uint8_t selector, uint32_t value)
{
    M4APluginData *data = (M4APluginData *)ctx;
    unsigned int meta = ((unsigned int)(uint8_t)trackIndex)
                      | ((unsigned int)selector << 8);

    atomic_store_explicit(&data->latestXcmdValue, value, memory_order_relaxed);
    atomic_store_explicit(&data->latestXcmdMeta, meta, memory_order_relaxed);
    atomic_fetch_add_explicit(&data->latestXcmdSeq, 1, memory_order_release);
}

static bool plugin_poll_pending_xcmd(M4APluginData *data, unsigned int *seq_out,
                                     unsigned int *meta_out)
{
    for (;;) {
        unsigned int seq1 = atomic_load_explicit(&data->pendingXcmdSeq, memory_order_acquire);
        unsigned int meta = atomic_load_explicit(&data->pendingXcmdMeta, memory_order_relaxed);
        unsigned int seq2 = atomic_load_explicit(&data->pendingXcmdSeq, memory_order_acquire);

        if (seq1 == seq2) {
            if (seq1 == 0 || seq1 == data->guiPendingXcmdSeqSeen)
                return false;
            *seq_out = seq1;
            *meta_out = meta;
            return true;
        }
    }
}

static bool plugin_poll_latest_xcmd(M4APluginData *data, unsigned int *seq_out,
                                    unsigned int *meta_out, unsigned int *value_out)
{
    for (;;) {
        unsigned int seq1 = atomic_load_explicit(&data->latestXcmdSeq, memory_order_acquire);
        unsigned int meta = atomic_load_explicit(&data->latestXcmdMeta, memory_order_relaxed);
        unsigned int value = atomic_load_explicit(&data->latestXcmdValue, memory_order_relaxed);
        unsigned int seq2 = atomic_load_explicit(&data->latestXcmdSeq, memory_order_acquire);

        if (seq1 == seq2) {
            if (seq1 == 0 || seq1 == data->guiLatestXcmdSeqSeen)
                return false;
            *seq_out = seq1;
            *meta_out = meta;
            *value_out = value;
            return true;
        }
    }
}

static void plugin_format_pending_xcmd(char *buf, size_t buf_size, unsigned int meta)
{
    unsigned int track = meta & 0xFFu;
    unsigned int selector = (meta >> 8) & 0xFFu;
    const char *name = NULL;

    switch (selector) {
    case 0x01: name = "xwave"; break;
    case 0x02: name = "xtype"; break;
    case 0x04: name = "xatta"; break;
    case 0x05: name = "xdeca"; break;
    case 0x06: name = "xsust"; break;
    case 0x07: name = "xrele"; break;
    case 0x08: name = "xiecv"; break;
    case 0x09: name = "xiecl"; break;
    case 0x0A: name = "xleng"; break;
    case 0x0B: name = "xswee"; break;
    case 0x0C: name = "xcmd_0C"; break;
    case 0x0D: name = "xcmd_0D"; break;
    default: break;
    }

    if (name)
        snprintf(buf, buf_size, "Ch %u: next CC29/31 -> %s", track + 1, name);
    else
        snprintf(buf, buf_size, "Ch %u: next CC29/31 -> XCMD 0x%02X", track + 1, selector);
}

/* ---- Audio processing ---- */

static clap_process_status plugin_process(const clap_plugin_t *plugin,
                                           const clap_process_t *process)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    if (!data->activated)
        return CLAP_PROCESS_ERROR;

    /* Read tempo from host transport when present (DAW with a CLAP transport).
     * In standalone — and in any host that doesn't supply a transport — tempo
     * comes from external MIDI clock instead; see process_midi_clock_pulse. */
    bool host_has_tempo = process->transport
        && (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO);
    if (host_has_tempo) {
        m4a_engine_set_tempo_bpm(&data->engine, process->transport->tempo);
        m4a_engine_recorder_set_tempo(&data->engine, 0, process->transport->tempo);
#if defined(M4A_DRIVER_V2)
        m4a_set_tempo_bpm(data->m4a_v2, process->transport->tempo);
#endif
        /* Host transport wins: drop any externally-derived clock state so we
         * don't fight it on the next clock pulse. */
        data->extClockInitialized = false;
    }

    /* Update recorder loop info from transport */
    if (process->transport
        && (process->transport->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE)
        && (process->transport->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)) {
        double startSec = (double)process->transport->loop_start_seconds / CLAP_SECTIME_FACTOR;
        double endSec   = (double)process->transport->loop_end_seconds   / CLAP_SECTIME_FACTOR;
        double posSec   = (double)process->transport->song_pos_seconds   / CLAP_SECTIME_FACTOR;
        m4a_engine_recorder_update_loop(&data->engine, true, startSec, endSec, posSec);
    } else if (process->transport
               && (process->transport->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)) {
        double posSec = (double)process->transport->song_pos_seconds / CLAP_SECTIME_FACTOR;
        m4a_engine_recorder_update_loop(&data->engine, false, 0.0, 0.0, posSec);
    }

    const uint32_t numFrames = process->frames_count;
    const uint32_t numEvents = process->in_events->size(process->in_events);

    /* Get output buffers */
    float *outL = process->audio_outputs[0].data32[0];
    float *outR = process->audio_outputs[0].data32[1];

    /* Process with sample-accurate event handling */
    uint32_t eventIdx = 0;
    uint32_t framePos = 0;

    while (framePos < numFrames) {
        /* Process all events at current position */
        while (eventIdx < numEvents) {
            const clap_event_header_t *hdr = process->in_events->get(process->in_events, eventIdx);
            if (hdr->time > framePos)
                break;

            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID) {
                switch (hdr->type) {
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE:
                    process_clap_note_event(data, (const clap_event_note_t *)hdr, hdr->time);
                    break;
                case CLAP_EVENT_MIDI:
                {
                    const clap_event_midi_t *midiEv = (const clap_event_midi_t *)hdr;
                    process_midi_event(data, midiEv->data, hdr->time);
                    /* MIDI Program Change is the source of truth when it
                     * arrives; mirror it back out as a CLAP param event so
                     * the host's automation lane and saved state stay in
                     * sync with the engine instead of clobbering it on the
                     * next params_flush. */
                    if ((midiEv->data[0] & 0xF0) == 0xC0
                        && process->out_events) {
                        uint8_t channel = midiEv->data[0] & 0x0F;
                        if (channel < MAX_TRACKS) {
                            clap_event_param_value_t pv;
                            memset(&pv, 0, sizeof(pv));
                            pv.header.size = sizeof(pv);
                            pv.header.time = hdr->time;
                            pv.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                            pv.header.type = CLAP_EVENT_PARAM_VALUE;
                            pv.header.flags = 0;
                            pv.param_id = channel;
                            pv.cookie = NULL;
                            pv.note_id = -1;
                            pv.port_index = -1;
                            pv.channel = -1;
                            pv.key = -1;
                            pv.value = (double)midiEv->data[1];
                            process->out_events->try_push(process->out_events,
                                                          &pv.header);
                        }
                    }
                    break;
                }
                case CLAP_EVENT_PARAM_VALUE:
                    m4a_params_process_event(data, (const clap_event_param_value_t *)hdr);
                    break;
                }
            }
            eventIdx++;
        }

        /* Determine how many frames to render before next event */
        uint32_t nextEventTime = numFrames;
        if (eventIdx < numEvents) {
            const clap_event_header_t *hdr = process->in_events->get(process->in_events, eventIdx);
            if (hdr->time < nextEventTime)
                nextEventTime = hdr->time;
        }

        uint32_t framesToRender = nextEventTime - framePos;
        if (framesToRender > 0) {
#if defined(M4A_DRIVER_V2) && defined(HW_AUDIO_V2)
            /* Cap each render-event-consume cycle at the driver's
             * recommended max so the bounded event queue never
             * overflows.  CLAP block sizes are usually small but the
             * spec doesn't bound them, so we chunk defensively. */
            uint32_t toGo = framesToRender;
            uint32_t off  = framePos;
            while (toGo > 0) {
                uint32_t chunk = toGo;
                if (chunk > (uint32_t)M4A_RECOMMENDED_MAX_ADVANCE_FRAMES)
                    chunk = (uint32_t)M4A_RECOMMENDED_MAX_ADVANCE_FRAMES;
                m4a_advance(data->m4a_v2, (int)chunk);
                hw_audio_render_events(data->hw_v2,
                                       m4a_get_pending_writes(data->m4a_v2),
                                       m4a_get_pcm_ring(data->m4a_v2),
                                       outL + off, outR + off, (int)chunk);
                m4a_consume_writes(data->m4a_v2);
                off  += chunk;
                toGo -= chunk;
            }
#else
            m4a_engine_process(&data->engine, outL + framePos, outR + framePos,
                              (int)framesToRender);
#endif
        }

        framePos = nextEventTime;
    }

    /* Advance the recorder's sample clock by the full block. Runs every
     * block — the duration display reflects total elapsed session time, not
     * the captured buffer length. The Record toggle only gates push_event,
     * so disarmed periods leave wall-time gaps in the SMF rather than
     * compressing the timeline. */
    m4a_engine_recorder_advance(&data->engine, numFrames);

    /* Tick the running sample-time counter used to measure MIDI clock intervals.
     * Sample_in_block offsets stay valid because realtime events are stamped
     * with the offset within *this* block before we add numFrames here. */
    data->extClockSampleCounter += numFrames;

    return CLAP_PROCESS_CONTINUE;
}

/* ---- Extensions ---- */

/* Audio ports extension */
static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    return is_input ? 0 : 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                            clap_audio_port_info_t *info)
{
    if (is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "Audio Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {
    .count = audio_ports_count,
    .get = audio_ports_get,
};

/* Note ports extension */
static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    return is_input ? 1 : 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                           clap_note_port_info_t *info)
{
    if (!is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "MIDI Input");
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    return true;
}

static const clap_plugin_note_ports_t s_note_ports = {
    .count = note_ports_count,
    .get = note_ports_get,
};

/* State extension - save/load voicegroup configuration */
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    /* Write a simple format: lengths + strings + parameters */
    uint32_t rootLen = (uint32_t)strlen(data->projectRoot);
    uint32_t nameLen = (uint32_t)strlen(data->voicegroupName);

    if (stream->write(stream, &rootLen, sizeof(rootLen)) != sizeof(rootLen)) return false;
    if (rootLen > 0 && stream->write(stream, data->projectRoot, rootLen) != (int64_t)rootLen) return false;
    if (stream->write(stream, &nameLen, sizeof(nameLen)) != sizeof(nameLen)) return false;
    if (nameLen > 0 && stream->write(stream, data->voicegroupName, nameLen) != (int64_t)nameLen) return false;
    if (stream->write(stream, &data->reverbAmount, 1) != 1) return false;
    if (stream->write(stream, &data->masterVolume, 1) != 1) return false;
    if (stream->write(stream, &data->songMasterVolume, 1) != 1) return false;
    uint8_t analogFilterByte = data->analogFilter ? 1 : 0;
    if (stream->write(stream, &analogFilterByte, 1) != 1) return false;
    if (stream->write(stream, &data->maxPcmChannels, 1) != 1) return false;
    if (!m4a_params_state_save(data, stream)) return false;

    /* Recorder state (optional block — old loads simply won't have it) */
    uint8_t armed = atomic_load(&data->recorderArmed) ? 1 : 0;
    if (stream->write(stream, &armed, 1) != 1) return false;
    uint32_t pathLen = (uint32_t)strlen(data->recorderPath);
    if (stream->write(stream, &pathLen, sizeof(pathLen)) != sizeof(pathLen)) return false;
    if (pathLen > 0 && stream->write(stream, data->recorderPath, pathLen) != (int64_t)pathLen) return false;
    uint8_t monoOutputByte = data->monoOutput ? 1 : 0;
    if (stream->write(stream, &monoOutputByte, 1) != 1) return false;

    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    /* Snapshot current voicegroup identity to detect changes after load */
    char prevRoot[sizeof(data->projectRoot)];
    char prevName[sizeof(data->voicegroupName)];
    memcpy(prevRoot, data->projectRoot,    sizeof(prevRoot));
    memcpy(prevName, data->voicegroupName, sizeof(prevName));

    uint32_t rootLen, nameLen;

    if (stream->read(stream, &rootLen, sizeof(rootLen)) != sizeof(rootLen)) return false;
    if (rootLen >= sizeof(data->projectRoot)) return false;
    if (rootLen > 0 && stream->read(stream, data->projectRoot, rootLen) != (int64_t)rootLen) return false;
    data->projectRoot[rootLen] = '\0';

    if (stream->read(stream, &nameLen, sizeof(nameLen)) != sizeof(nameLen)) return false;
    if (nameLen >= sizeof(data->voicegroupName)) return false;
    if (nameLen > 0 && stream->read(stream, data->voicegroupName, nameLen) != (int64_t)nameLen) return false;
    data->voicegroupName[nameLen] = '\0';

    if (stream->read(stream, &data->reverbAmount, 1) != 1) return false;
    if (stream->read(stream, &data->masterVolume, 1) != 1) return false;
    if (stream->read(stream, &data->songMasterVolume, 1) != 1) return false;
    /* analogFilter byte is optional (not present in older saves); default to enabled */
    uint8_t analogFilterByte = 1;
    stream->read(stream, &analogFilterByte, 1);
    data->analogFilter = (analogFilterByte != 0);
    /* maxPcmChannels byte is optional (not present in older saves); default to 5 */
    uint8_t maxChannelsByte = 5;
    stream->read(stream, &maxChannelsByte, 1);
    if (maxChannelsByte < 1) maxChannelsByte = 1;
    if (maxChannelsByte > MAX_PCM_CHANNELS) maxChannelsByte = MAX_PCM_CHANNELS;
    data->maxPcmChannels = maxChannelsByte;
    m4a_params_state_load(data, stream);

    /* Recorder state (optional — EOF here means an older save; don't fail) */
    {
        uint8_t armed = 0;
        if (stream->read(stream, &armed, 1) == 1) {
            atomic_store(&data->recorderArmed, armed != 0);
            uint32_t pathLen = 0;
            if (stream->read(stream, &pathLen, sizeof(pathLen)) == (int64_t)sizeof(pathLen)
                && pathLen < sizeof(data->recorderPath)) {
                if (pathLen > 0)
                    stream->read(stream, data->recorderPath, pathLen);
                data->recorderPath[pathLen] = '\0';
            }
        }
    }
    {
        uint8_t monoOutputByte = data->monoOutput ? 1 : 0;
        if (stream->read(stream, &monoOutputByte, 1) == 1)
            data->monoOutput = (monoOutputByte != 0);
    }

    if (data->activated) {
        /* Only reload voicegroup if the project root or name actually changed */
        bool vgChanged = strcmp(data->projectRoot,    prevRoot) != 0 ||
                         strcmp(data->voicegroupName, prevName) != 0;
        if (vgChanged && data->projectRoot[0] && data->voicegroupName[0]) {
            if (data->loadedVg) {
                voicegroup_free(data->loadedVg);
                data->loadedVg = NULL;
            }
            data->loadedVg = voicegroup_load(data->projectRoot, data->voicegroupName,
                                             &data->loaderConfig);
            if (data->loadedVg) {
                m4a_engine_set_voicegroup(&data->engine, data->loadedVg->voices);
#if defined(M4A_DRIVER_V2)
                m4a_driver_set_voicegroup(data->m4a_v2, data->loadedVg->voices);
#endif
                memcpy(data->originalVoices, data->loadedVg->voices, sizeof(data->originalVoices));
                memset(data->voiceOverrides, 0, sizeof(data->voiceOverrides));
                voicegroup_state_write_default(data->projectRoot,
                                           data->voicegroupName,
                                           data->loadedVg);
            }
        }
        data->engine.masterVolume = data->masterVolume;
        data->engine.songMasterVolume = data->songMasterVolume;
        data->engine.analogFilter = data->analogFilter;
        data->engine.maxPcmChannels = data->maxPcmChannels;
        m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);
#if defined(M4A_DRIVER_V2)
        m4a_set_master_volume(data->m4a_v2, data->masterVolume);
        m4a_set_song_volume(data->m4a_v2, data->songMasterVolume);
        m4a_set_analog_filter(data->m4a_v2, data->analogFilter);
        m4a_set_stereo(data->m4a_v2, !data->monoOutput);
        m4a_set_max_pcm_channels(data->m4a_v2, data->maxPcmChannels);
        m4a_set_reverb_amount(data->m4a_v2, data->reverbAmount);
#endif
        m4a_params_sync_to_engine(data);
    }

    /* Push restored values into the GUI so it reflects the loaded state */
    if (data->gui) {
        M4AGuiSettings gs;
        memset(&gs, 0, sizeof(gs));
        snprintf(gs.projectRoot,    sizeof(gs.projectRoot),    "%s", data->projectRoot);
        snprintf(gs.voicegroupName, sizeof(gs.voicegroupName), "%s", data->voicegroupName);
        gs.reverbAmount     = data->reverbAmount;
        gs.masterVolume     = data->masterVolume;
        gs.songMasterVolume = data->songMasterVolume;
        gs.analogFilter     = data->analogFilter;
        gs.monoOutput       = data->monoOutput;
        gs.maxPcmChannels   = data->maxPcmChannels;
        gs.voicegroupLoaded = (data->loadedVg != NULL);
        m4a_gui_update_settings(data->gui, &gs);
        if (data->loadedVg)
            m4a_gui_set_voice_data(data->gui, data->loadedVg->voices, data->originalVoices, data->voiceOverrides);
        else
            m4a_gui_set_voice_data(data->gui, NULL, NULL, NULL);
    }

    return true;
}

static const clap_plugin_state_t s_state = {
    .save = state_save,
    .load = state_load,
};

/* ---- GUI extension ---- */

static void plugin_log(const char *fmt, ...)
{
    if (!s_pluginLogPath) return;
    FILE *f = fopen(s_pluginLogPath, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- Timer support extension ---- */

static void timer_on_timer(const clap_plugin_t *plugin, clap_id timer_id)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (!data->gui)
        return;
    /* If the host supports timers, only respond to our registered timer.
     * If not (e.g. standalone without native timer support), accept any id
     * so an external driver can call on_timer to pump the GUI. */
    if (data->guiTimerId != CLAP_INVALID_ID && timer_id != data->guiTimerId)
        return;

    for (int ch = 0; ch < MAX_TRACKS; ch++) {
        unsigned int midiSeq = atomic_load(&data->midiActivitySeq[ch]);
        if (midiSeq != data->guiMidiActivitySeqSeen[ch]) {
            data->guiMidiActivitySeqSeen[ch] = midiSeq;
            m4a_gui_pulse_midi_activity(data->gui, ch);
        }
    }

    {
        unsigned int xcmdSeq = atomic_load(&data->xcmdActivitySeq);
        if (xcmdSeq != data->guiXcmdActivitySeqSeen) {
            data->guiXcmdActivitySeqSeen = xcmdSeq;
            m4a_gui_pulse_xcmd_activity(data->gui);
        }
    }

    {
        unsigned int seq, meta;

        if (plugin_poll_pending_xcmd(data, &seq, &meta)) {
            char pending_xcmd[128];

            plugin_format_pending_xcmd(pending_xcmd, sizeof(pending_xcmd), meta);
            m4a_gui_set_latest_xcmd(data->gui, pending_xcmd);
            data->guiPendingXcmdSeqSeen = seq;
        }
    }

    {
        unsigned int seq, meta, value;

        if (plugin_poll_latest_xcmd(data, &seq, &meta, &value)) {
            (void)meta;
            (void)value;
            m4a_gui_pulse_valid_xcmd(data->gui);
            data->guiLatestXcmdSeqSeen = seq;
        }
    }

    /* Render one GUI frame */
    m4a_gui_tick(data->gui);

    /* Handle voice restore requests from the voice editor */
    int restoreIdx;
    bool voicesChanged = false;
    while (m4a_gui_poll_voice_restore(data->gui, &restoreIdx)) {
        if (data->loadedVg && restoreIdx >= 0 && restoreIdx < VOICEGROUP_SIZE) {
            data->loadedVg->voices[restoreIdx] = data->originalVoices[restoreIdx];
            data->voiceOverrides[restoreIdx] = false;
            voicesChanged = true;
        }
    }

    /* If any voice was edited or restored, refresh active tracks */
    if (m4a_gui_poll_voices_dirty(data->gui))
        voicesChanged = true;
    if (voicesChanged && data->activated) {
        m4a_engine_refresh_voices(&data->engine);
#if defined(M4A_DRIVER_V2)
        m4a_driver_refresh_voices(data->m4a_v2);
#endif
    }

    /* Handle sample swap requests from the voice editor */
    {
        int swapVoice;
        ProjectAssetKind swapKind;
        char swapFileName[256];
        while (m4a_gui_poll_sample_swap(data->gui, &swapVoice, &swapKind, swapFileName, sizeof(swapFileName))) {
            if (data->assetIndex) {
                project_asset_index_set_override(data->assetIndex, swapVoice, swapKind, swapFileName);
                data->restartRequested = true;
                data->host->request_restart(data->host);
            }
        }
    }

    /* Apply any settings the user changed */
    M4AGuiSettings gs;
    bool reloadVoicegroup = false;
    if (!m4a_gui_poll_changes(data->gui, &gs, &reloadVoicegroup))
        return;

    /* Immediate audio settings - safe to write since they're byte-sized */
    data->reverbAmount     = gs.reverbAmount;
    data->masterVolume     = gs.masterVolume;
    data->songMasterVolume = gs.songMasterVolume;
    data->analogFilter     = gs.analogFilter;
    data->monoOutput       = gs.monoOutput;
    data->maxPcmChannels   = gs.maxPcmChannels;

    if (data->activated) {
        data->engine.masterVolume = gs.masterVolume;
        m4a_engine_set_song_volume(&data->engine, gs.songMasterVolume);
        m4a_reverb_set_amount(&data->engine.reverb, gs.reverbAmount);
        data->engine.analogFilter = gs.analogFilter;
        data->engine.maxPcmChannels = gs.maxPcmChannels;
#if defined(M4A_DRIVER_V2)
        m4a_set_master_volume(data->m4a_v2, gs.masterVolume);
        m4a_set_song_volume(data->m4a_v2, gs.songMasterVolume);
        m4a_set_reverb_amount(data->m4a_v2, gs.reverbAmount);
        m4a_set_analog_filter(data->m4a_v2, gs.analogFilter);
        m4a_set_stereo(data->m4a_v2, !gs.monoOutput);
        m4a_set_max_pcm_channels(data->m4a_v2, gs.maxPcmChannels);
#endif
    }

    if (reloadVoicegroup) {
        /* Update paths, then ask the host to deactivate/reactivate so the new
         * voicegroup is loaded cleanly from the audio thread's perspective. */
        snprintf(data->projectRoot,    sizeof(data->projectRoot),
                 "%s", gs.projectRoot);
        snprintf(data->voicegroupName, sizeof(data->voicegroupName),
                 "%s", gs.voicegroupName);
        /* Reload clears all sample overrides — user gets fresh voicegroup */
        if (data->assetIndex)
            project_asset_index_clear_all_overrides(data->assetIndex);
        data->restartRequested = true;
        data->host->request_restart(data->host);
    }

    /* Register this change with the host's undo stack.
     *
     * Prefer the CLAP undo draft extension when available: pass no delta so
     * the host snapshots state via state->save()/state->load().
     *
     * Fall back to mark_dirty() for hosts (e.g. Reaper) that don't implement
     * the draft extension. Per the CLAP spec, mark_dirty() creates an implicit
     * undo step as long as the plugin hasn't opted into CLAP_EXT_UNDO. */
    const clap_host_undo_t *hostUndo =
        (const clap_host_undo_t *)data->host->get_extension(data->host, CLAP_EXT_UNDO);
    if (hostUndo && hostUndo->change_made) {
        const char *name = reloadVoicegroup ? "M4A: Reload Voicegroup"
                                            : "M4A: Settings Change";
        hostUndo->change_made(data->host, name, NULL, 0, false);
    } else {
        const clap_host_state_t *hostState =
            (const clap_host_state_t *)data->host->get_extension(data->host, CLAP_EXT_STATE);
        if (hostState && hostState->mark_dirty)
            hostState->mark_dirty(data->host);
    }

    /* Reflect updated status back into the GUI (voicegroupLoaded may change
     * after request_restart completes, but update the rest immediately). */
    gs.voicegroupLoaded = (data->loadedVg != NULL);
    m4a_gui_update_settings(data->gui, &gs);
}

static void gui_internal_timer_callback(void *user_data)
{
    /* This timer_id pretending to
    look like a CLAP timer ID from the host
    looks like its arbitrarily 0...could be a bug in the future. */
    timer_on_timer((const clap_plugin_t *)user_data, 0);
}

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    (void)plugin;
    bool supported = false;
    if (is_floating) {
        supported = true;
    } else if (api) {
#if defined(_WIN32)
        supported = (strcmp(api, CLAP_WINDOW_API_WIN32) == 0);
#elif defined(__APPLE__)
        supported = (strcmp(api, CLAP_WINDOW_API_COCOA) == 0);
#else
        supported = (strcmp(api, CLAP_WINDOW_API_X11) == 0);
#endif
    }
    plugin_log("gui_is_api_supported: api=%s floating=%d -> %d",
               api ? api : "(null)", is_floating, supported);
    return supported;
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin,
                                  const char **api, bool *is_floating)
{
    (void)plugin;
    /* Prefer embedded on all platforms */
    *is_floating = false;
#if defined(_WIN32)
    *api = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    *api = CLAP_WINDOW_API_COCOA;
#else
    *api = CLAP_WINDOW_API_X11;
#endif
    return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    plugin_log("gui_create: api=%s floating=%d", api ? api : "(null)", is_floating);
    (void)api;
    (void)is_floating;

    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    M4AGuiSettings gs;
    memset(&gs, 0, sizeof(gs));
    snprintf(gs.projectRoot,    sizeof(gs.projectRoot),    "%s", data->projectRoot);
    snprintf(gs.voicegroupName, sizeof(gs.voicegroupName), "%s", data->voicegroupName);
    gs.reverbAmount     = data->reverbAmount;
    gs.masterVolume     = data->masterVolume;
    gs.songMasterVolume = data->songMasterVolume;
    gs.analogFilter     = data->analogFilter;
    gs.monoOutput       = data->monoOutput;
    gs.maxPcmChannels   = data->maxPcmChannels;
    gs.voicegroupLoaded = (data->loadedVg != NULL);

    data->gui = m4a_gui_create(data->host, &gs, s_pluginLogPath);
    if (!data->gui) {
        plugin_log("gui_create: m4a_gui_create() returned NULL");
        return false;
    }
    m4a_gui_set_internal_timer_callback(data->gui, gui_internal_timer_callback, (void *)plugin);

    /* Wire plugin data pointer for the recorder tab */
    m4a_gui_set_plugin_data(data->gui, data);

    /* Wire voice data pointers if voicegroup is already loaded */
    if (data->loadedVg)
        m4a_gui_set_voice_data(data->gui, data->loadedVg->voices, data->originalVoices, data->voiceOverrides);

    /* Wire project asset catalog for the sample selector */
    if (data->assetIndex)
        m4a_gui_set_project_assets(data->gui,
                                   data->assetIndex->directsoundAssets, data->assetIndex->directsoundCount,
                                   data->assetIndex->progWaveAssets, data->assetIndex->progWaveCount,
                                   data->assetIndex->overrides);

    plugin_log("gui_create: success");

    /* Register a ~60 Hz timer to drive GUI rendering */
    const clap_host_timer_support_t *timerExt =
        (const clap_host_timer_support_t *)data->host->get_extension(
            data->host, CLAP_EXT_TIMER_SUPPORT);

    return true;
}

static void gui_destroy(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (!data->gui)
        return;

    /* Unregister the render timer */
    if (data->guiTimerId != CLAP_INVALID_ID) {
        const clap_host_timer_support_t *timerExt =
            (const clap_host_timer_support_t *)data->host->get_extension(
                data->host, CLAP_EXT_TIMER_SUPPORT);

        data->guiTimerId = CLAP_INVALID_ID;
    }

    m4a_gui_destroy(data->gui);
    data->gui = NULL;
}

static bool gui_set_scale(const clap_plugin_t *plugin, double scale)
{
    (void)plugin;
    (void)scale;
    return false; /* Pugl handles DPI internally */
}

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_gui_get_size(data->gui, width, height);
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_can_resize(data->gui);
}

static bool gui_get_resize_hints(const clap_plugin_t *plugin,
                                 clap_gui_resize_hints_t *hints)
{
    (void)plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically   = true;
    hints->preserve_aspect_ratio   = false;
    return true;
}

static bool gui_adjust_size(const clap_plugin_t *plugin,
                            uint32_t *width, uint32_t *height)
{
    /* Accept any size the host offers */
    (void)plugin;
    (void)width;
    (void)height;
    return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_set_size(data->gui, width, height);
}

static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    plugin_log("gui_set_parent called");
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    uintptr_t parent = 0;
#if defined(_WIN32)
    parent = (uintptr_t)window->win32;
#elif defined(__APPLE__)
    parent = (uintptr_t)window->cocoa;
#else
    parent = (uintptr_t)window->x11;
#endif
    return m4a_gui_set_parent(data->gui, parent);
}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
    (void)plugin;
    (void)window;
    return true;
}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
    (void)plugin;
    (void)title;
}

static bool gui_show(const clap_plugin_t *plugin)
{
    plugin_log("gui_show called");
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    bool ok = m4a_gui_show(data->gui);

    /* If the host doesn't provide timer_support, start Pugl's internal
     * NSTimer to drive rendering at ~60 Hz. Must be called after
     * gui_show (view must be realized and visible). */
    if (ok && data->guiTimerId == CLAP_INVALID_ID)
        m4a_gui_start_internal_timer(data->gui);

    return ok;
}

static bool gui_hide(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_hide(data->gui);
}

static const clap_plugin_gui_t s_gui = {
    .is_api_supported  = gui_is_api_supported,
    .get_preferred_api = gui_get_preferred_api,
    .create            = gui_create,
    .destroy           = gui_destroy,
    .set_scale         = gui_set_scale,
    .get_size          = gui_get_size,
    .can_resize        = gui_can_resize,
    .get_resize_hints  = gui_get_resize_hints,
    .adjust_size       = gui_adjust_size,
    .set_size          = gui_set_size,
    .set_parent        = gui_set_parent,
    .set_transient     = gui_set_transient,
    .suggest_title     = gui_suggest_title,
    .show              = gui_show,
    .hide              = gui_hide,
};

static const clap_plugin_timer_support_t s_timer_support = {
    .on_timer = timer_on_timer,
};

/* Standalone helper: check and clear the restart-requested flag */
bool m4a_plugin_take_restart_request(const clap_plugin_t *plugin)
{
    if (!plugin) return false;
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (!data->restartRequested) return false;
    data->restartRequested = false;
    return true;
}

/* Standalone helper: check if the plugin's GUI window was closed by the user */
bool m4a_plugin_gui_was_closed(const clap_plugin_t *plugin)
{
    if (!plugin) return false;
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return data->gui && m4a_gui_was_closed(data->gui);
}

/* Standalone helper: preferred audio output device name from poryaaaa.cfg
 * (`audio_output=` key). Returns an empty string if unset — caller should
 * then fall back to the system default. Returned pointer is owned by the
 * plugin and valid for the plugin's lifetime. */
const char *m4a_plugin_audio_output_name(const clap_plugin_t *plugin)
{
    if (!plugin) return "";
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return data->audioOutputName;
}

/* Standalone helper: wire audio-device callbacks into the GUI so the
 * Settings tab can render an output-device dropdown. CLAP-in-DAW callers
 * should leave this unset — the host owns audio routing there. */
void m4a_plugin_set_audio_api(const clap_plugin_t *plugin, const M4AHostAudioApi *api)
{
    if (!plugin) return;
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (data && data->gui)
        m4a_gui_set_audio_api(data->gui, api);
}

/* Extension dispatcher */
static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)   return &s_audio_ports;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)    return &s_note_ports;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0)        return m4a_params_extension();
    if (strcmp(id, CLAP_EXT_STATE) == 0)          return &s_state;
    if (strcmp(id, CLAP_EXT_GUI) == 0)            return &s_gui;
    if (strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)  return &s_timer_support;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
}

/* ---- Factory ---- */

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
    return 1;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(
    const clap_plugin_factory_t *factory, uint32_t index)
{
    if (index == 0) return &s_descriptor;
    return NULL;
}

static const clap_plugin_t *factory_create_plugin(
    const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
    if (strcmp(plugin_id, s_descriptor.id) != 0)
        return NULL;

    M4APluginData *data = calloc(1, sizeof(M4APluginData));
    if (!data) return NULL;

    data->host = host;

    clap_plugin_t *plugin = calloc(1, sizeof(clap_plugin_t));
    if (!plugin) {
        free(data);
        return NULL;
    }

    plugin->desc = &s_descriptor;
    plugin->plugin_data = data;
    plugin->init = plugin_init;
    plugin->destroy = plugin_destroy;
    plugin->activate = plugin_activate;
    plugin->deactivate = plugin_deactivate;
    plugin->start_processing = plugin_start_processing;
    plugin->stop_processing = plugin_stop_processing;
    plugin->reset = plugin_reset;
    plugin->process = plugin_process;
    plugin->get_extension = plugin_get_extension;
    plugin->on_main_thread = plugin_on_main_thread;

    return plugin;
}

static const clap_plugin_factory_t s_factory = {
    .get_plugin_count = factory_get_plugin_count,
    .get_plugin_descriptor = factory_get_plugin_descriptor,
    .create_plugin = factory_create_plugin,
};

/* ---- Entry point ---- */

static bool entry_init(const char *plugin_path)
{
    if (plugin_path && plugin_path[0]) {
        /* Extract directory portion: everything up to the last separator */
        const char *end = plugin_path + strlen(plugin_path);
        while (end > plugin_path && *end != '/' && *end != '\\')
            end--;
        if (end > plugin_path) {
            size_t dirLen = (size_t)(end - plugin_path);
            if (dirLen >= sizeof(s_pluginDir))
                dirLen = sizeof(s_pluginDir) - 1;
            memcpy(s_pluginDir, plugin_path, dirLen);
            s_pluginDir[dirLen] = '\0';
        }

#ifdef __APPLE__
        /* On macOS the binary lives at <bundle>.clap/Contents/MacOS/<binary>.
         * The cfg file should sit next to the bundle, not inside it, so
         * navigate up two levels: Contents/MacOS -> Contents -> bundle root,
         * then one more to the directory that contains the bundle. */
        {
            char *p = s_pluginDir;
            size_t len = strlen(p);
            /* Check suffix .../Contents/MacOS (case-sensitive on macOS) */
            const char *suffix = "/Contents/MacOS";
            size_t slen = strlen(suffix);
            if (len > slen && strcmp(p + len - slen, suffix) == 0) {
                /* Strip /Contents/MacOS to get the bundle root */
                p[len - slen] = '\0';
                /* Strip the bundle name (.clap dir) to get the install dir */
                char *last = p + strlen(p);
                while (last > p && *last != '/')
                    last--;
                if (last > p)
                    *last = '\0';
            }
        }
#endif
    }
    return true;
}

static void entry_deinit(void)
{
}

static const void *entry_get_factory(const char *factory_id)
{
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &s_factory;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
