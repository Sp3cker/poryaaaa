#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/draft/undo.h>
#include "m4a_plugin.h"
#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_reverb.h"
#include "voicegroup_loader.h"
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
 * Directory of the loaded plugin binary or bundle, set during entry_init.
 * Used to find poryaaaa.cfg in the same directory as the plugin.
 */
static char s_pluginDir[512] = {0};

/* Optional diagnostic log path, set from config key "log=<path>" */
static const char *s_pluginLogPath = NULL;

/*
 * Load settings from poryaaaa.cfg placed next to the plugin install.
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
        } else if (strcmp(key, "max_channels") == 0) {
            int v = atoi(value);
            if (v < 1) v = 1;
            if (v > MAX_PCM_CHANNELS) v = MAX_PCM_CHANNELS;
            data->maxPcmChannels = (uint8_t)v;
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
        } else if (strcmp(key, "sample_dirs") == 0) {
            char tmp[600];
            strncpy(tmp, value, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *tok = strtok(tmp, ";");
            while (tok && data->loaderConfig.sampleDirCount < 8) {
                while (*tok == ' ') tok++;
                int idx = data->loaderConfig.sampleDirCount++;
                snprintf(data->loaderConfig.sampleDirs[idx],
                         sizeof(data->loaderConfig.sampleDirs[idx]), "%s", tok);
                tok = strtok(NULL, ";");
            }
        }
    }

    fclose(f);
}

/* ---- Plugin lifecycle ---- */

/* Forward declaration: apply a parameter change to the engine */
static void apply_param_value(M4APluginData *data, clap_id paramId, double value);
static void flush_pending_param_events(M4APluginData *data,
                                       const clap_output_events_t *out,
                                       uint32_t time);

static uint8_t clamp_param_value(clap_id paramId, double value)
{
    int v = (int)(value + 0.5);

    switch (paramId) {
    case PARAM_VOLUME:
    case PARAM_PAN:
    case PARAM_MOD_DEPTH:
    case PARAM_LFO_SPEED:
    case PARAM_REVERB:
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        break;
    case PARAM_BEND_RANGE:
        if (v < 1) v = 1;
        if (v > 24) v = 24;
        break;
    default:
        v = 0;
        break;
    }

    return (uint8_t)v;
}

static double get_param_value(const M4APluginData *data, clap_id paramId)
{
    if (paramId >= PARAM_COUNT)
        return 0.0;
    return (double)atomic_load(&data->paramValues[paramId]);
}

static void queue_param_output(M4APluginData *data, clap_id paramId, uint8_t value,
                               uint32_t eventFlags, bool withGesture)
{
    if (paramId >= PARAM_COUNT)
        return;

    atomic_store(&data->pendingParamValues[paramId], value);
    atomic_fetch_or(&data->pendingParamEventFlags[paramId], eventFlags);
    if (withGesture)
        atomic_store(&data->pendingParamGestures[paramId], true);
    atomic_store(&data->pendingParamOutputs[paramId], true);
}

static bool push_param_gesture_event(const clap_output_events_t *out, clap_id paramId,
                                     uint32_t time, bool isBegin)
{
    if (!out || !out->try_push)
        return false;

    clap_event_param_gesture_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.header.size = sizeof(ev);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = isBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
    ev.param_id = paramId;
    return out->try_push(out, &ev.header);
}

static bool push_param_value_event(const clap_output_events_t *out, clap_id paramId,
                                   double value, uint32_t time, uint32_t flags)
{
    if (!out || !out->try_push)
        return false;

    clap_event_param_value_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.header.size = sizeof(ev);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.flags = flags;
    ev.param_id = paramId;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.note_id = -1;
    ev.value = value;
    return out->try_push(out, &ev.header);
}

static void request_host_param_flush(M4APluginData *data)
{
    if (!data->host)
        return;

    const clap_host_params_t *hostParams =
        (const clap_host_params_t *)data->host->get_extension(data->host, CLAP_EXT_PARAMS);
    if (hostParams && hostParams->request_flush) {
        hostParams->request_flush(data->host);
    } else if (data->host->request_process) {
        data->host->request_process(data->host);
    }
}

static bool plugin_init(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    data->masterVolume = 15;
    data->songMasterVolume = MAX_SONG_VOLUME;
    data->reverbAmount = 0;
    data->analogFilter = false;
    data->maxPcmChannels = 5;
    data->projectRoot[0] = '\0';
    data->voicegroupName[0] = '\0';
    data->loadedVg = NULL;
    data->activated = false;
    data->gui = NULL;
    data->guiTimerId = CLAP_INVALID_ID;
    /* Default automation parameter values */
    for (int i = 0; i < PARAM_COUNT; i++) {
        atomic_init(&data->paramValues[i], 0);
        atomic_init(&data->pendingParamValues[i], 0);
        atomic_init(&data->pendingParamEventFlags[i], 0);
        atomic_init(&data->pendingParamOutputs[i], false);
        atomic_init(&data->pendingParamGestures[i], false);
    }
    atomic_store(&data->paramValues[PARAM_VOLUME], 127);
    atomic_store(&data->paramValues[PARAM_PAN], 64);
    atomic_store(&data->paramValues[PARAM_MOD_DEPTH], 0);
    atomic_store(&data->paramValues[PARAM_BEND_RANGE], 2);
    atomic_store(&data->paramValues[PARAM_LFO_SPEED], 0);
    atomic_store(&data->paramValues[PARAM_REVERB], 0);
    /* Load defaults from config file placed next to the plugin */
    load_config_file(data);
    /* Forward the log path into the voicegroup loader so it can emit diagnostics */
    voicegroup_loader_set_log_path(s_pluginLogPath);
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
    m4a_engine_destroy(&data->engine);
    free(data);
    free((void *)plugin);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate,
                            uint32_t min_frames, uint32_t max_frames)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_init(&data->engine, (float)sample_rate);
    data->engine.masterVolume = data->masterVolume;
    data->engine.songMasterVolume = data->songMasterVolume;
    data->engine.analogFilter = data->analogFilter;
    data->engine.maxPcmChannels = data->maxPcmChannels;
    m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);

    /* Apply automation parameter defaults to track 0 */
    atomic_store(&data->paramValues[PARAM_REVERB], data->reverbAmount);
    for (int i = 0; i < PARAM_COUNT; i++)
        apply_param_value(data, (clap_id)i, get_param_value(data, (clap_id)i));

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
            memcpy(data->originalVoices, data->loadedVg->voices, sizeof(data->originalVoices));
            memset(data->voiceOverrides, 0, sizeof(data->voiceOverrides));
        }
    }

    data->activated = true;

    /* Update voice data pointers for the GUI */
    if (data->gui) {
        if (data->loadedVg)
            m4a_gui_set_voice_data(data->gui, data->loadedVg->voices, data->originalVoices, data->voiceOverrides);
        else
            m4a_gui_set_voice_data(data->gui, NULL, NULL, NULL);
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
    m4a_reverb_reset(&data->engine.reverb);
    data->engine.lowPassLeft  = 0.0f;
    data->engine.lowPassRight = 0.0f;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_all_sound_off(&data->engine);
    m4a_reverb_reset(&data->engine.reverb);
    data->engine.lowPassLeft  = 0.0f;
    data->engine.lowPassRight = 0.0f;
}

/* ---- Parameter helpers ---- */

/* Apply a CLAP parameter change to the engine (audio-thread). */
static void apply_param_value(M4APluginData *data, clap_id paramId, double value)
{
    if (paramId >= PARAM_COUNT)
        return;
    uint8_t v = clamp_param_value(paramId, value);
    atomic_store(&data->paramValues[paramId], v);

    switch (paramId) {
    case PARAM_VOLUME:
        m4a_engine_cc(&data->engine, 0, 0x7, v);
        break;
    case PARAM_PAN:
        m4a_engine_cc(&data->engine, 0, 0xA, v);
        break;
    case PARAM_MOD_DEPTH:
        m4a_engine_cc(&data->engine, 0, 0x1, v);
        break;
    case PARAM_BEND_RANGE:
        m4a_engine_cc(&data->engine, 0, 0x14, v);
        break;
    case PARAM_LFO_SPEED:
        m4a_engine_cc(&data->engine, 0, 0x15, v);
        break;
    case PARAM_REVERB:
        data->reverbAmount = v;
        m4a_reverb_set_amount(&data->engine.reverb, v);
        break;
    }
}

static clap_id param_id_from_cc(uint8_t cc)
{
    switch (cc) {
    case 0x7:  return PARAM_VOLUME;
    case 0xA:  return PARAM_PAN;
    case 0x1:  return PARAM_MOD_DEPTH;
    case 0x14: return PARAM_BEND_RANGE;
    case 0x15: return PARAM_LFO_SPEED;
    }

    return CLAP_INVALID_ID;
}

static void flush_pending_param_events(M4APluginData *data,
                                       const clap_output_events_t *out,
                                       uint32_t time)
{
    for (clap_id paramId = 0; paramId < PARAM_COUNT; paramId++) {
        if (!atomic_exchange(&data->pendingParamOutputs[paramId], false))
            continue;

        const uint8_t rawValue = atomic_load(&data->pendingParamValues[paramId]);
        const uint32_t flags = atomic_exchange(&data->pendingParamEventFlags[paramId], 0);
        const bool withGesture = atomic_exchange(&data->pendingParamGestures[paramId], false);

        if (withGesture)
            push_param_gesture_event(out, paramId, time, true);
        if (!push_param_value_event(out, paramId, (double)rawValue, time, flags))
            queue_param_output(data, paramId, rawValue, flags, withGesture);
        else if (withGesture)
            push_param_gesture_event(out, paramId, time, false);
    }
}

/* Process input events that are CLAP parameter changes. */
static void process_param_events(M4APluginData *data,
                                 const clap_input_events_t *in,
                                 const clap_output_events_t *out)
{
    const uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; i++) {
        const clap_event_header_t *hdr = in->get(in, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t *ev = (const clap_event_param_value_t *)hdr;
            apply_param_value(data, ev->param_id, ev->value);
        }
    }
    flush_pending_param_events(data, out, 0);
}

/* ---- Params extension ---- */

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return PARAM_COUNT;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t paramIndex,
                            clap_param_info_t *info)
{
    (void)plugin;
    memset(info, 0, sizeof(*info));
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_REQUIRES_PROCESS;

    switch (paramIndex) {
    case PARAM_VOLUME:
        info->id = PARAM_VOLUME;
        snprintf(info->name, sizeof(info->name), "Volume");
        info->min_value     = 0;
        info->max_value     = 127;
        info->default_value = 127;
        break;
    case PARAM_PAN:
        info->id = PARAM_PAN;
        snprintf(info->name, sizeof(info->name), "Pan");
        info->min_value     = 0;
        info->max_value     = 127;
        info->default_value = 64;
        break;
    case PARAM_MOD_DEPTH:
        info->id = PARAM_MOD_DEPTH;
        snprintf(info->name, sizeof(info->name), "Vibrato Depth");
        info->min_value     = 0;
        info->max_value     = 127;
        info->default_value = 0;
        break;
    case PARAM_BEND_RANGE:
        info->id = PARAM_BEND_RANGE;
        snprintf(info->name, sizeof(info->name), "Bend Range");
        info->min_value     = 1;
        info->max_value     = 24;
        info->default_value = 2;
        break;
    case PARAM_LFO_SPEED:
        info->id = PARAM_LFO_SPEED;
        snprintf(info->name, sizeof(info->name), "LFO Speed");
        info->min_value     = 0;
        info->max_value     = 127;
        info->default_value = 0;
        break;
    case PARAM_REVERB:
        info->id = PARAM_REVERB;
        snprintf(info->name, sizeof(info->name), "Reverb");
        info->min_value     = 0;
        info->max_value     = 127;
        info->default_value = 0;
        break;
    default:
        return false;
    }
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id paramId, double *outValue)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (paramId >= PARAM_COUNT)
        return false;
    *outValue = get_param_value(data, paramId);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id paramId,
                                 double value, char *outBuffer, uint32_t outBufferCapacity)
{
    (void)plugin;
    int v = (int)(value + 0.5);

    if (paramId == PARAM_PAN) {
        int pan = v - 64;
        if (pan < 0)
            snprintf(outBuffer, outBufferCapacity, "L%d", -pan);
        else if (pan > 0)
            snprintf(outBuffer, outBufferCapacity, "R%d", pan);
        else
            snprintf(outBuffer, outBufferCapacity, "C");
    } else if (paramId == PARAM_BEND_RANGE) {
        snprintf(outBuffer, outBufferCapacity, "%d st", v);
    } else {
        snprintf(outBuffer, outBufferCapacity, "%d", v);
    }
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id paramId,
                                 const char *text, double *outValue)
{
    (void)plugin;
    if (!text || !outValue)
        return false;

    if (paramId == PARAM_PAN) {
        if (text[0] == 'C' || text[0] == 'c') {
            *outValue = clamp_param_value(paramId, 64);
            return true;
        }
        if (text[0] == 'L' || text[0] == 'l') {
            *outValue = clamp_param_value(paramId, 64 - atoi(text + 1));
            return true;
        }
        if (text[0] == 'R' || text[0] == 'r') {
            *outValue = clamp_param_value(paramId, 64 + atoi(text + 1));
            return true;
        }
    }

    *outValue = clamp_param_value(paramId, atof(text));
    return true;
}

static void params_flush(const clap_plugin_t *plugin,
                          const clap_input_events_t *in,
                          const clap_output_events_t *out)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    process_param_events(data, in, out);
}

static const clap_plugin_params_t s_params = {
    .count         = params_count,
    .get_info      = params_get_info,
    .get_value     = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush         = params_flush,
};

/* ---- MIDI event processing ---- */

/*
 * Single-instrument mode: all incoming MIDI is routed to track 0.
 * The MIDI channel byte is ignored so the plugin behaves as one
 * instrument instance per DAW track.
 */
static void process_midi_event(M4APluginData *data, const uint8_t *msg,
                               const clap_output_events_t *out,
                               uint32_t time, uint32_t eventFlags)
{
    uint8_t status = msg[0] & 0xF0;

    switch (status) {
    case 0x90: /* Note On */
        if (msg[2] > 0) {
            m4a_engine_note_on(&data->engine, 0, msg[1], msg[2]);
        } else {
            /* velocity 0 = note off */
            m4a_engine_note_off(&data->engine, 0, msg[1]);
        }
        break;
    case 0x80: /* Note Off */
        m4a_engine_note_off(&data->engine, 0, msg[1]);
        break;
    case 0xC0: /* Program Change */
        m4a_engine_program_change(&data->engine, 0, msg[1]);
        break;
    case 0xB0: /* Control Change */
        m4a_engine_cc(&data->engine, 0, msg[1], msg[2]);
        {
            clap_id paramId = param_id_from_cc(msg[1]);
            if (paramId != CLAP_INVALID_ID) {
                atomic_store(&data->paramValues[paramId], clamp_param_value(paramId, msg[2]));
                if (!push_param_value_event(out, paramId, get_param_value(data, paramId), time,
                                            eventFlags | CLAP_EVENT_DONT_RECORD)) {
                    queue_param_output(data, paramId,
                                       (uint8_t)get_param_value(data, paramId),
                                       eventFlags | CLAP_EVENT_DONT_RECORD, false);
                }
            }
        }
        break;
    case 0xE0: /* Pitch Bend */
    {
        int16_t bend = ((int16_t)msg[2] << 7 | msg[1]) - 8192;
        m4a_engine_pitch_bend(&data->engine, 0, bend);
        break;
    }
    }
}

static void process_clap_note_event(M4APluginData *data, const clap_event_note_t *ev)
{
    if (ev->header.type == CLAP_EVENT_NOTE_ON) {
        uint8_t velocity = (uint8_t)(ev->velocity * 127.0 + 0.5);
        if (velocity == 0) velocity = 1;
        m4a_engine_note_on(&data->engine, 0, (uint8_t)ev->key, velocity);
    } else if (ev->header.type == CLAP_EVENT_NOTE_OFF) {
        m4a_engine_note_off(&data->engine, 0, (uint8_t)ev->key);
    } else if (ev->header.type == CLAP_EVENT_NOTE_CHOKE) {
        m4a_engine_note_off(&data->engine, 0, (uint8_t)ev->key);
    }
}

/* ---- Audio processing ---- */

static clap_process_status plugin_process(const clap_plugin_t *plugin,
                                           const clap_process_t *process)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    if (!data->activated)
        return CLAP_PROCESS_ERROR;

    /* Read tempo from host transport (MIDI meta event tempo) */
    if (process->transport
        && (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO)) {
        m4a_engine_set_tempo_bpm(&data->engine, process->transport->tempo);
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
                    process_clap_note_event(data, (const clap_event_note_t *)hdr);
                    break;
                case CLAP_EVENT_PARAM_VALUE:
                {
                    const clap_event_param_value_t *pv = (const clap_event_param_value_t *)hdr;
                    apply_param_value(data, pv->param_id, pv->value);
                    break;
                }
                case CLAP_EVENT_MIDI:
                {
                    const clap_event_midi_t *midiEv = (const clap_event_midi_t *)hdr;
                    process_midi_event(data, midiEv->data, process->out_events,
                                       hdr->time, hdr->flags & CLAP_EVENT_IS_LIVE);
                    break;
                }
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
            m4a_engine_process(&data->engine, outL + framePos, outR + framePos,
                              (int)framesToRender);
        }

        framePos = nextEventTime;
    }

    flush_pending_param_events(data, process->out_events, 0);

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

    /* Automation parameter values (v2 state) */
    for (int i = 0; i < PARAM_COUNT; i++) {
        double v = get_param_value(data, (clap_id)i);
        if (stream->write(stream, &v, sizeof(v)) != sizeof(v)) return false;
    }

    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    uint8_t prevParamValues[PARAM_COUNT];
    for (int i = 0; i < PARAM_COUNT; i++)
        prevParamValues[i] = (uint8_t)get_param_value(data, (clap_id)i);

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
    atomic_store(&data->paramValues[PARAM_REVERB], data->reverbAmount);
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

    /* Automation parameter values (v2 state, optional for backward compat) */
    {
        double pv;
        for (int i = 0; i < PARAM_COUNT; i++) {
            if (stream->read(stream, &pv, sizeof(pv)) == sizeof(pv))
                atomic_store(&data->paramValues[i], clamp_param_value((clap_id)i, pv));
        }
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
                memcpy(data->originalVoices, data->loadedVg->voices, sizeof(data->originalVoices));
                memset(data->voiceOverrides, 0, sizeof(data->voiceOverrides));
            }
        }
        data->engine.masterVolume = data->masterVolume;
        data->engine.songMasterVolume = data->songMasterVolume;
        data->engine.analogFilter = data->analogFilter;
        data->engine.maxPcmChannels = data->maxPcmChannels;
        m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);

        /* Re-apply automation params to track 0 */
        for (int i = 0; i < PARAM_COUNT; i++)
            apply_param_value(data, (clap_id)i, get_param_value(data, (clap_id)i));
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
        gs.maxPcmChannels   = data->maxPcmChannels;
        gs.voicegroupLoaded = (data->loadedVg != NULL);
        m4a_gui_update_settings(data->gui, &gs);
        if (data->loadedVg)
            m4a_gui_set_voice_data(data->gui, data->loadedVg->voices, data->originalVoices, data->voiceOverrides);
        else
            m4a_gui_set_voice_data(data->gui, NULL, NULL, NULL);
    }

    bool paramValuesChanged = false;
    for (int i = 0; i < PARAM_COUNT; i++) {
        if (prevParamValues[i] != (uint8_t)get_param_value(data, (clap_id)i)) {
            paramValuesChanged = true;
            break;
        }
    }
    if (paramValuesChanged) {
        const clap_host_params_t *hostParams =
            (const clap_host_params_t *)data->host->get_extension(data->host, CLAP_EXT_PARAMS);
        if (hostParams && hostParams->rescan)
            hostParams->rescan(data->host, CLAP_PARAM_RESCAN_VALUES);
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
    gs.maxPcmChannels   = data->maxPcmChannels;
    gs.voicegroupLoaded = (data->loadedVg != NULL);

    data->gui = m4a_gui_create(data->host, &gs, s_pluginLogPath);
    if (!data->gui) {
        plugin_log("gui_create: m4a_gui_create() returned NULL");
        return false;
    }

    /* Wire voice data pointers if voicegroup is already loaded */
    if (data->loadedVg)
        m4a_gui_set_voice_data(data->gui, data->loadedVg->voices, data->originalVoices, data->voiceOverrides);

    plugin_log("gui_create: success");

    /* Register a ~60 Hz timer to drive GUI rendering */
    const clap_host_timer_support_t *timerExt =
        (const clap_host_timer_support_t *)data->host->get_extension(
            data->host, CLAP_EXT_TIMER_SUPPORT);
    if (timerExt)
        timerExt->register_timer(data->host, 16 /* ms */, &data->guiTimerId);

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
        if (timerExt)
            timerExt->unregister_timer(data->host, data->guiTimerId);
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
    return m4a_gui_show(data->gui);
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
    if (voicesChanged && data->activated)
        m4a_engine_refresh_voices(&data->engine);

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
    data->maxPcmChannels   = gs.maxPcmChannels;

    if (data->activated) {
        data->engine.masterVolume = gs.masterVolume;
        m4a_engine_set_song_volume(&data->engine, gs.songMasterVolume);
        m4a_reverb_set_amount(&data->engine.reverb, gs.reverbAmount);
        data->engine.analogFilter = gs.analogFilter;
        data->engine.maxPcmChannels = gs.maxPcmChannels;
    }

    uint8_t reverbParamValue = (uint8_t)get_param_value(data, PARAM_REVERB);
    if (gs.reverbAmount != reverbParamValue) {
        atomic_store(&data->paramValues[PARAM_REVERB], gs.reverbAmount);
        queue_param_output(data, PARAM_REVERB, gs.reverbAmount, CLAP_EVENT_IS_LIVE, true);
        request_host_param_flush(data);
    }

    if (reloadVoicegroup) {
        /* Update paths, then ask the host to deactivate/reactivate so the new
         * voicegroup is loaded cleanly from the audio thread's perspective. */
        snprintf(data->projectRoot,    sizeof(data->projectRoot),
                 "%s", gs.projectRoot);
        snprintf(data->voicegroupName, sizeof(data->voicegroupName),
                 "%s", gs.voicegroupName);
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

/* Extension dispatcher */
static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)   return &s_audio_ports;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)    return &s_note_ports;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0)         return &s_params;
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
