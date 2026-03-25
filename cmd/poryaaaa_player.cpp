#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#ifdef __linux__
#include <dlfcn.h>
#endif

#include "m4a_engine.h"
#include "m4a_reverb.h"
#include "midi_timeline.h"
#include "native_file_dialog.h"
#include "voicegroup_loader.h"

typedef struct {
    char projectRoot[512];
    char voicegroupName[256];
    uint8_t reverbAmount;
    uint8_t songMasterVolume;
    bool analogFilter;
    uint8_t maxPcmChannels;
    bool voicegroupLoaded;
} PlayerSettings;

typedef struct {
    char midiPath[512];
    bool midiLoaded;
    bool isPlaying;
    double positionSeconds;
    double totalSeconds;
    bool trackUsed[MAX_TRACKS];
    bool trackMuted[MAX_TRACKS];
    uint8_t trackChannels[MAX_TRACKS];
    uint8_t trackPrograms[MAX_TRACKS];
} PlaybackState;

typedef struct {
    PlayerSettings settings;
    PlaybackState playback;

    char projectRootBuf[512];
    char voicegroupBuf[256];
    char midiPathBuf[512];
    char configPath[1024];
    char configMidiPath[512];
    VoicegroupNameList *voicegroupChoices;

    M4AEngine engine;
    bool engineInitialized;

    LoadedVoiceGroup *loadedVg;
    MidiTimeline *timeline;
    ToneData originalVoices[VOICEGROUP_SIZE];
    bool voiceOverrides[VOICEGROUP_SIZE];
    int selectedVoice;

    ma_device device;
    bool deviceInitialized;

    std::mutex mutex;

    double sampleRate;
    double tailSeconds;
    uint64_t totalPlaybackSamples;
    uint64_t currentSample;
    int nextEventIndex;
    float scratchL[4096];
    float scratchR[4096];
} PlayerApp;

static bool reload_voicegroup(PlayerApp *app);

static bool file_exists(const char *path)
{
    if (!path || !path[0])
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static void trim_trailing_whitespace(char *s)
{
    size_t len = strlen(s);
    while (len > 0) {
        const char c = s[len - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
            break;
        s[--len] = '\0';
    }
}

static char *trim_leading_whitespace(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void load_config_file(PlayerApp *app, const char *configPath,
                             char *startupMidiPath, size_t startupMidiPathSize)
{
    if (!configPath || !configPath[0])
        return;

    FILE *f = fopen(configPath, "r");
    if (!f)
        return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim_trailing_whitespace(line);
        char *trimmed = trim_leading_whitespace(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0')
            continue;

        char *eq = strchr(trimmed, '=');
        if (!eq)
            continue;
        *eq = '\0';

        char *key = trim_leading_whitespace(trimmed);
        char *value = trim_leading_whitespace(eq + 1);
        trim_trailing_whitespace(key);
        trim_trailing_whitespace(value);

        if (strcmp(key, "project_root") == 0) {
            snprintf(app->settings.projectRoot, sizeof(app->settings.projectRoot), "%s", value);
        } else if (strcmp(key, "voicegroup") == 0) {
            snprintf(app->settings.voicegroupName, sizeof(app->settings.voicegroupName), "%s", value);
        } else if (strcmp(key, "reverb") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            app->settings.reverbAmount = (uint8_t)v;
        } else if (strcmp(key, "song_master_volume") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            app->settings.songMasterVolume = (uint8_t)v;
        } else if (strcmp(key, "analog_filter") == 0) {
            app->settings.analogFilter = (atoi(value) != 0);
        } else if (strcmp(key, "max_channels") == 0) {
            int v = atoi(value);
            if (v < 1) v = 1;
            if (v > MAX_PCM_CHANNELS) v = MAX_PCM_CHANNELS;
            app->settings.maxPcmChannels = (uint8_t)v;
        } else if (strcmp(key, "sample_rate") == 0) {
            int v = atoi(value);
            if (v >= 8000)
                app->sampleRate = (double)v;
        } else if (strcmp(key, "tail") == 0) {
            double v = atof(value);
            if (v >= 0.0)
                app->tailSeconds = v;
        } else if (strcmp(key, "midi") == 0 && startupMidiPath && startupMidiPathSize > 0 && value[0]) {
            snprintf(app->configMidiPath, sizeof(app->configMidiPath), "%s", value);
            snprintf(startupMidiPath, startupMidiPathSize, "%s", value);
        }
    }

    fclose(f);
}

static void resolve_config_path(char *outPath, size_t outPathSize, const char *argv0)
{
    outPath[0] = '\0';

    if (file_exists("poryaaaa.cfg")) {
        snprintf(outPath, outPathSize, "%s", "poryaaaa.cfg");
        return;
    }

    if (!argv0 || !argv0[0])
        return;

    std::error_code ec;
    std::filesystem::path exePath = std::filesystem::absolute(argv0, ec);
    if (ec)
        return;

    std::filesystem::path cfgPath = exePath.parent_path() / "poryaaaa.cfg";
    const std::string cfgString = cfgPath.string();
    if (!file_exists(cfgString.c_str()))
        return;

    snprintf(outPath, outPathSize, "%s", cfgString.c_str());
}

static const char *voice_type_name(uint8_t type)
{
    uint8_t base = type & ~VOICE_TYPE_FIX;
    switch (base) {
    case 0x00: return "DirectSound";
    case 0x01: return "Square 1";
    case 0x02: return "Square 2";
    case 0x03: return "Prog Wave";
    case 0x04: return "Noise";
    case VOICE_CRY:          return "Cry";
    case VOICE_CRY_REVERSE:  return "Cry (Reverse)";
    case VOICE_KEYSPLIT:     return "Keysplit";
    case VOICE_KEYSPLIT_ALL: return "Drum Kit";
    default: return "Unknown";
    }
}

static bool edit_directsound_adsr(ToneData *voice)
{
    bool changed = false;
    int a = voice->attack, d = voice->decay, s = voice->sustain, r = voice->release;
    if (ImGui::SliderInt("Attack##ds", &a, 0, 255))  { voice->attack  = (uint8_t)a; changed = true; }
    if (ImGui::SliderInt("Decay##ds",  &d, 0, 255))  { voice->decay   = (uint8_t)d; changed = true; }
    if (ImGui::SliderInt("Sustain##ds",&s, 0, 255))  { voice->sustain = (uint8_t)s; changed = true; }
    if (ImGui::SliderInt("Release##ds",&r, 0, 255))  { voice->release = (uint8_t)r; changed = true; }
    return changed;
}

static bool edit_cgb_adsr(ToneData *voice)
{
    bool changed = false;
    int a = voice->attack, d = voice->decay, s = voice->sustain, r = voice->release;
    if (ImGui::SliderInt("Attack##cgb", &a, 0, 7))   { voice->attack  = (uint8_t)a; changed = true; }
    if (ImGui::SliderInt("Decay##cgb",  &d, 0, 7))   { voice->decay   = (uint8_t)d; changed = true; }
    if (ImGui::SliderInt("Sustain##cgb",&s, 0, 15))  { voice->sustain = (uint8_t)s; changed = true; }
    if (ImGui::SliderInt("Release##cgb",&r, 0, 7))   { voice->release = (uint8_t)r; changed = true; }
    return changed;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [project_root] [voicegroup] [options]\n"
            "\n"
            "Defaults may be loaded from poryaaaa.cfg in the current working\n"
            "directory or next to the executable.\n"
            "\n"
            "Options:\n"
            "  --midi <file.mid>        Load a MIDI file on startup\n"
            "  --song-volume <0-127>    Song master volume (default: 127)\n"
            "  --reverb <0-127>         Reverb amount (default: 0)\n"
            "  --analog-filter          Enable GBA analog low-pass filter\n"
            "  --polyphony <1-12>       Max simultaneous PCM channels (default: 5)\n"
            "  --sample-rate <hz>       Audio sample rate (default: 44100)\n"
            "  --tail <seconds>         Silence after last MIDI event (default: 3.0)\n",
            prog);
}

#ifdef __linux__
typedef int (*snd_lib_error_set_handler_fn)(void (*handler)(const char *, int, const char *, int, const char *, ...));

static void null_alsa_error_handler(const char *, int, const char *, int, const char *, ...)
{
}

static void suppress_alsa_errors(void)
{
    void *alsa = dlopen("libasound.so.2", RTLD_LAZY);
    if (!alsa) return;

    snd_lib_error_set_handler_fn setHandler =
        (snd_lib_error_set_handler_fn)dlsym(alsa, "snd_lib_error_set_handler");
    if (setHandler)
        setHandler(null_alsa_error_handler);
}
#endif

static void sync_input_buffers(PlayerApp *app)
{
    snprintf(app->projectRootBuf, sizeof(app->projectRootBuf), "%s", app->settings.projectRoot);
    snprintf(app->voicegroupBuf, sizeof(app->voicegroupBuf), "%s", app->settings.voicegroupName);
    snprintf(app->midiPathBuf, sizeof(app->midiPathBuf), "%s", app->playback.midiPath);
}

static void refresh_voicegroup_choices(PlayerApp *app, const char *projectRoot)
{
    voicegroup_name_list_free(app->voicegroupChoices);
    app->voicegroupChoices = voicegroup_name_list_discover(projectRoot, NULL);
}

static bool line_matches_key(const std::string &line, const char *key)
{
    const size_t pos = line.find('=');
    if (pos == std::string::npos)
        return false;

    size_t start = 0;
    while (start < pos && (line[start] == ' ' || line[start] == '\t'))
        start++;

    size_t end = pos;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t'))
        end--;

    return line.compare(start, end - start, key) == 0;
}

static void upsert_config_line(std::vector<std::string> *lines, const char *key, const char *value)
{
    const std::string entry = std::string(key) + "=" + (value ? value : "");
    for (std::string &line : *lines) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line_matches_key(line, key)) {
            line = entry;
            return;
        }
    }
    lines->push_back(entry);
}

static void save_config_file(const PlayerApp *app)
{
    if (!app || !app->configPath[0])
        return;

    std::vector<std::string> lines;
    {
        std::ifstream in(app->configPath);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
    }

    upsert_config_line(&lines, "project_root", app->settings.projectRoot);
    upsert_config_line(&lines, "voicegroup", app->settings.voicegroupName);

    char numberBuf[64];

    snprintf(numberBuf, sizeof(numberBuf), "%u", (unsigned)app->settings.reverbAmount);
    upsert_config_line(&lines, "reverb", numberBuf);

    snprintf(numberBuf, sizeof(numberBuf), "%u", (unsigned)app->settings.songMasterVolume);
    upsert_config_line(&lines, "song_master_volume", numberBuf);

    snprintf(numberBuf, sizeof(numberBuf), "%u", app->settings.analogFilter ? 1U : 0U);
    upsert_config_line(&lines, "analog_filter", numberBuf);

    snprintf(numberBuf, sizeof(numberBuf), "%u", (unsigned)app->settings.maxPcmChannels);
    upsert_config_line(&lines, "max_channels", numberBuf);

    snprintf(numberBuf, sizeof(numberBuf), "%.0f", app->sampleRate);
    upsert_config_line(&lines, "sample_rate", numberBuf);

    snprintf(numberBuf, sizeof(numberBuf), "%.3f", app->tailSeconds);
    upsert_config_line(&lines, "tail", numberBuf);

    upsert_config_line(&lines, "midi", app->configMidiPath);

    std::ofstream out(app->configPath, std::ios::trunc);
    if (!out)
        return;

    for (size_t i = 0; i < lines.size(); i++) {
        out << lines[i];
        if (i + 1 < lines.size())
            out << '\n';
    }
}

static void apply_project_selection(PlayerApp *app)
{
    snprintf(app->settings.projectRoot, sizeof(app->settings.projectRoot), "%s", app->projectRootBuf);
    snprintf(app->settings.voicegroupName, sizeof(app->settings.voicegroupName), "%s", app->voicegroupBuf);
    refresh_voicegroup_choices(app, app->settings.projectRoot);
    reload_voicegroup(app);
    save_config_file(app);
}

static void apply_engine_settings(PlayerApp *app)
{
    m4a_engine_set_song_volume(&app->engine, app->settings.songMasterVolume);
    m4a_reverb_set_amount(&app->engine.reverb, app->settings.reverbAmount);
    app->engine.analogFilter = app->settings.analogFilter;
    app->engine.maxPcmChannels = app->settings.maxPcmChannels;
}

static void init_or_reset_engine_locked(PlayerApp *app)
{
    if (app->engineInitialized)
        m4a_engine_destroy(&app->engine);

    m4a_engine_init(&app->engine, (float)app->sampleRate);
    app->engineInitialized = true;
    if (app->loadedVg)
        m4a_engine_set_voicegroup(&app->engine, app->loadedVg->voices);
    apply_engine_settings(app);
}

static void reset_transport_locked(PlayerApp *app, bool startPlaying)
{
    bool muted[MAX_TRACKS];
    memcpy(muted, app->playback.trackMuted, sizeof(muted));

    init_or_reset_engine_locked(app);

    memcpy(app->playback.trackMuted, muted, sizeof(app->playback.trackMuted));
    memset(app->playback.trackPrograms, 0, sizeof(app->playback.trackPrograms));
    app->currentSample = 0;
    app->nextEventIndex = 0;
    app->playback.positionSeconds = 0.0;
    app->playback.isPlaying = startPlaying && app->timeline && app->loadedVg;
}

static void dispatch_event_locked(PlayerApp *app, const MidiRenderEvent *ev)
{
    if (!app->timeline) return;

    int trackIdx = midi_timeline_track_index(app->timeline, ev);
    if (trackIdx < 0 || trackIdx >= MAX_TRACKS)
        return;

    app->playback.trackChannels[trackIdx] = ev->channel;

    if (app->playback.trackMuted[trackIdx] && ev->type == 0x9)
        return;

    switch (ev->type) {
    case 0x8:
        m4a_engine_note_off(&app->engine, trackIdx, ev->data0);
        break;
    case 0x9:
        m4a_engine_note_on(&app->engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xB:
        m4a_engine_cc(&app->engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xC:
        app->playback.trackPrograms[trackIdx] = ev->data0;
        m4a_engine_program_change(&app->engine, trackIdx, ev->data0);
        break;
    case 0xE:
    {
        int16_t bend = (int16_t)(((int)(ev->data1 << 7) | ev->data0) - 8192);
        m4a_engine_pitch_bend(&app->engine, trackIdx, bend);
        break;
    }
    }
}

static void audio_callback(ma_device *device, void *output, const void *, ma_uint32 frameCount)
{
    PlayerApp *app = (PlayerApp *)device->pUserData;
    float *dst = (float *)output;

    for (ma_uint32 i = 0; i < frameCount * 2; i++)
        dst[i] = 0.0f;

    std::lock_guard<std::mutex> lock(app->mutex);

    if (!app->playback.isPlaying || !app->timeline || !app->loadedVg)
        return;

    ma_uint32 remaining = frameCount;
    ma_uint32 offset = 0;
    while (remaining > 0) {
        if (app->currentSample >= app->totalPlaybackSamples) {
            app->playback.isPlaying = false;
            m4a_engine_all_sound_off(&app->engine);
            break;
        }

        while (app->nextEventIndex < app->timeline->count &&
               app->timeline->events[app->nextEventIndex].samplePos <= app->currentSample) {
            dispatch_event_locked(app, &app->timeline->events[app->nextEventIndex]);
            app->nextEventIndex++;
        }

        uint64_t nextBoundary = app->totalPlaybackSamples;
        if (app->nextEventIndex < app->timeline->count)
            nextBoundary = app->timeline->events[app->nextEventIndex].samplePos;

        uint64_t framesUntilBoundary = nextBoundary > app->currentSample
                                     ? nextBoundary - app->currentSample
                                     : 0;
        ma_uint32 chunk = remaining;
        if (framesUntilBoundary > 0 && framesUntilBoundary < chunk)
            chunk = (ma_uint32)framesUntilBoundary;

        if (chunk == 0) {
            dispatch_event_locked(app, &app->timeline->events[app->nextEventIndex]);
            app->nextEventIndex++;
            continue;
        }

        ma_uint32 rendered = 0;
        while (rendered < chunk) {
            ma_uint32 slice = chunk - rendered;
            if (slice > (ma_uint32)(sizeof(app->scratchL) / sizeof(app->scratchL[0])))
                slice = (ma_uint32)(sizeof(app->scratchL) / sizeof(app->scratchL[0]));
            m4a_engine_process(&app->engine, app->scratchL, app->scratchR, (int)slice);
            for (ma_uint32 i = 0; i < slice; i++) {
                dst[(offset + rendered + i) * 2] = app->scratchL[i];
                dst[(offset + rendered + i) * 2 + 1] = app->scratchR[i];
            }
            rendered += slice;
        }

        app->currentSample += chunk;
        offset += chunk;
        remaining -= chunk;
    }

    app->playback.positionSeconds = (double)app->currentSample / app->sampleRate;
}

static bool reload_voicegroup(PlayerApp *app)
{
    LoadedVoiceGroup *newVg = voicegroup_load(app->settings.projectRoot,
                                              app->settings.voicegroupName,
                                              NULL);
    if (!newVg) {
        app->settings.voicegroupLoaded = (app->loadedVg != NULL);
        return false;
    }

    std::lock_guard<std::mutex> lock(app->mutex);

    if (app->loadedVg)
        voicegroup_free(app->loadedVg);
    app->loadedVg = newVg;
    memcpy(app->originalVoices, app->loadedVg->voices, sizeof(app->originalVoices));
    memset(app->voiceOverrides, 0, sizeof(app->voiceOverrides));
    init_or_reset_engine_locked(app);
    if (app->timeline)
        reset_transport_locked(app, false);
    app->settings.voicegroupLoaded = true;
    return true;
}

static bool load_midi(PlayerApp *app, const char *path)
{
    MidiTimeline *timeline = midi_timeline_load(path, app->sampleRate);
    if (!timeline)
        return false;

    std::lock_guard<std::mutex> lock(app->mutex);

    midi_timeline_free(app->timeline);
    app->timeline = timeline;
    snprintf(app->playback.midiPath, sizeof(app->playback.midiPath), "%s", path);
    memset(app->playback.trackUsed, 0, sizeof(app->playback.trackUsed));
    memset(app->playback.trackChannels, 0, sizeof(app->playback.trackChannels));
    memset(app->playback.trackPrograms, 0, sizeof(app->playback.trackPrograms));

    for (int i = 0; i < timeline->count; i++) {
        int trackIdx = midi_timeline_track_index(timeline, &timeline->events[i]);
        if (trackIdx < 0 || trackIdx >= MAX_TRACKS)
            continue;
        app->playback.trackUsed[trackIdx] = true;
        app->playback.trackChannels[trackIdx] = timeline->events[i].channel;
    }

    app->totalPlaybackSamples = timeline->totalMidiSamples
                              + (uint64_t)(app->tailSeconds * app->sampleRate + 0.5);
    app->playback.midiLoaded = true;
    app->playback.totalSeconds = (double)app->totalPlaybackSamples / app->sampleRate;
    snprintf(app->configMidiPath, sizeof(app->configMidiPath), "%s", path);
    reset_transport_locked(app, false);
    save_config_file(app);
    return true;
}

static void apply_track_mute(PlayerApp *app, int trackIdx, bool muted)
{
    std::lock_guard<std::mutex> lock(app->mutex);
    bool wasMuted = app->playback.trackMuted[trackIdx];
    app->playback.trackMuted[trackIdx] = muted;
    if (!wasMuted && muted)
        m4a_engine_all_notes_off(&app->engine, trackIdx);
}

static void seek_transport_locked(PlayerApp *app, double seconds)
{
    if (!app->timeline || !app->loadedVg)
        return;

    if (seconds < 0.0)
        seconds = 0.0;
    if (app->playback.totalSeconds > 0.0 && seconds > app->playback.totalSeconds)
        seconds = app->playback.totalSeconds;

    const bool wasPlaying = app->playback.isPlaying;
    const uint64_t targetSample = (uint64_t)(seconds * app->sampleRate + 0.5);

    reset_transport_locked(app, false);
    app->currentSample = targetSample > app->totalPlaybackSamples
                       ? app->totalPlaybackSamples
                       : targetSample;

    while (app->nextEventIndex < app->timeline->count &&
           app->timeline->events[app->nextEventIndex].samplePos <= app->currentSample) {
        dispatch_event_locked(app, &app->timeline->events[app->nextEventIndex]);
        app->nextEventIndex++;
    }

    app->playback.positionSeconds = (double)app->currentSample / app->sampleRate;
    app->playback.isPlaying = wasPlaying && app->currentSample < app->totalPlaybackSamples;
}

static void render_general_tab(PlayerApp *app)
{
    ImGui::SeparatorText("Project Settings");

    ImGui::Text("Project Root");
    {
        const float buttonWidth = 80.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - spacing);
    }
    ImGui::InputText("##root", app->projectRootBuf, sizeof(app->projectRootBuf));
    ImGui::SameLine();
    if (ImGui::Button("Browse##root", ImVec2(80.0f, 0.0f))) {
        char chosenPath[sizeof(app->projectRootBuf)];
        if (choose_directory_dialog(chosenPath, sizeof(chosenPath))) {
            snprintf(app->projectRootBuf, sizeof(app->projectRootBuf), "%s", chosenPath);
            refresh_voicegroup_choices(app, app->projectRootBuf);
        }
    }

    ImGui::Text("Voicegroup");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::InputText("##voicegroup", app->voicegroupBuf, sizeof(app->voicegroupBuf));
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(80.0f, 0.0f))) {
        apply_project_selection(app);
    }

    ImGui::Text("Available");
    {
        const float buttonWidth = 80.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - spacing);
    }
    const char *preview = app->voicegroupBuf[0]
        ? app->voicegroupBuf
        : ((app->voicegroupChoices && app->voicegroupChoices->count > 0)
            ? "<select voicegroup>"
            : "<no voicegroups found>");
    if (ImGui::BeginCombo("##voicegroupChoices", preview)) {
        if (app->voicegroupChoices) {
            for (int i = 0; i < app->voicegroupChoices->count; i++) {
                const char *name = app->voicegroupChoices->names[i];
                const bool selected = strcmp(name, app->voicegroupBuf) == 0;
                if (ImGui::Selectable(name, selected)) {
                    snprintf(app->voicegroupBuf, sizeof(app->voicegroupBuf), "%s", name);
                    apply_project_selection(app);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh##voicegroup", ImVec2(80.0f, 0.0f)))
        refresh_voicegroup_choices(app, app->projectRootBuf);

    ImGui::Text("Status: %s", app->settings.voicegroupLoaded ? "Voicegroup loaded" : "Voicegroup not loaded");

    ImGui::Spacing();
    ImGui::SeparatorText("Audio Settings");

    int songVolume = (int)app->settings.songMasterVolume;
    if (ImGui::SliderInt("Song Volume", &songVolume, 0, 127)) {
        app->settings.songMasterVolume = (uint8_t)songVolume;
        std::lock_guard<std::mutex> lock(app->mutex);
        apply_engine_settings(app);
        save_config_file(app);
    }

    int reverb = (int)app->settings.reverbAmount;
    if (ImGui::SliderInt("Reverb", &reverb, 0, 127)) {
        app->settings.reverbAmount = (uint8_t)reverb;
        std::lock_guard<std::mutex> lock(app->mutex);
        apply_engine_settings(app);
        save_config_file(app);
    }

    int polyphony = (int)app->settings.maxPcmChannels;
    if (ImGui::SliderInt("Polyphony", &polyphony, 1, MAX_PCM_CHANNELS)) {
        app->settings.maxPcmChannels = (uint8_t)polyphony;
        std::lock_guard<std::mutex> lock(app->mutex);
        apply_engine_settings(app);
        save_config_file(app);
    }

    if (ImGui::Checkbox("GBA Analog Filter", &app->settings.analogFilter)) {
        std::lock_guard<std::mutex> lock(app->mutex);
        apply_engine_settings(app);
        save_config_file(app);
    }
}

static void render_player_tab(PlayerApp *app)
{
    ImGui::SeparatorText("MIDI File");
    const float buttonWidth = 80.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (buttonWidth * 2.0f) - (spacing * 2.0f));
    ImGui::InputText("##midi", app->midiPathBuf, sizeof(app->midiPathBuf));
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(buttonWidth, 0.0f))) {
        char chosenPath[sizeof(app->midiPathBuf)];
        if (choose_midi_file_dialog(chosenPath, sizeof(chosenPath))) {
            snprintf(app->midiPathBuf, sizeof(app->midiPathBuf), "%s", chosenPath);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(buttonWidth, 0.0f))) {
        load_midi(app, app->midiPathBuf);
    }

    ImGui::Text("Status: %s", app->playback.midiLoaded ? "MIDI loaded" : "No MIDI loaded");

    ImGui::Spacing();
    ImGui::SeparatorText("Transport");

    if (ImGui::Button(app->playback.isPlaying ? "Pause" : "Play", ImVec2(90.0f, 0.0f))) {
        std::lock_guard<std::mutex> lock(app->mutex);
        if (app->timeline && app->loadedVg)
            app->playback.isPlaying = !app->playback.isPlaying;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(90.0f, 0.0f))) {
        std::lock_guard<std::mutex> lock(app->mutex);
        reset_transport_locked(app, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart", ImVec2(90.0f, 0.0f))) {
        std::lock_guard<std::mutex> lock(app->mutex);
        reset_transport_locked(app, true);
    }

    ImGui::Spacing();

    double playheadSeconds = app->playback.positionSeconds;
    const double minSeconds = 0.0;
    const double maxSeconds = app->playback.totalSeconds > 0.0
                            ? app->playback.totalSeconds
                            : 0.0;
    const bool canSeek = app->playback.midiLoaded && app->playback.totalSeconds > 0.0;

    ImGui::Text("Playhead");
    if (!canSeek)
        ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderScalar("##playhead", ImGuiDataType_Double,
                            &playheadSeconds, &minSeconds, &maxSeconds,
                            "%.2f s", ImGuiSliderFlags_NoRoundToFormat)) {
        std::lock_guard<std::mutex> lock(app->mutex);
        seek_transport_locked(app, playheadSeconds);
    }
    if (!canSeek)
        ImGui::EndDisabled();
    ImGui::Text("%.2f / %.2f s", app->playback.positionSeconds, app->playback.totalSeconds);

    ImGui::Spacing();
    ImGui::SeparatorText("Tracks");

    if (ImGui::BeginTable("##tracks", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Track");
        ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("Program");
        ImGui::TableHeadersRow();

        for (int i = 0; i < MAX_TRACKS; i++) {
            if (!app->playback.trackUsed[i])
                continue;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Track %d", i);

            ImGui::TableSetColumnIndex(1);
            bool muted = app->playback.trackMuted[i];
            char label[32];
            snprintf(label, sizeof(label), "##mute%d", i);
            if (ImGui::Checkbox(label, &muted))
                apply_track_mute(app, i, muted);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", (unsigned)app->playback.trackChannels[i] + 1U);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", (unsigned)app->playback.trackPrograms[i]);
        }

        ImGui::EndTable();
    }
}

static void render_voices_tab(PlayerApp *app)
{
    std::lock_guard<std::mutex> lock(app->mutex);

    if (!app->loadedVg) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "No voicegroup loaded");
        return;
    }

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::SliderInt("##voiceSlider", &app->selectedVoice, 0, 127);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##voiceInput", &app->selectedVoice, 1, 10);
    if (app->selectedVoice < 0) app->selectedVoice = 0;
    if (app->selectedVoice > 127) app->selectedVoice = 127;

    int idx = app->selectedVoice;
    ToneData *voice = &app->loadedVg->voices[idx];
    uint8_t type = voice->type;

    ImGui::Text("Type: %s (0x%02X)", voice_type_name(type), type);
    if (type == VOICE_DIRECTSOUND_NO_RESAMPLE) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[Fixed]");
    }

    if (app->voiceOverrides[idx]) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "(modified)");
    }

    ImGui::Separator();

    bool changed = false;
    uint8_t baseType = type & ~VOICE_TYPE_FIX;

    if (baseType == 0x00) {
        ImGui::Text("Key: %d", voice->key);
        ImGui::Text("Pan/Sweep: %d (0x%02X)", voice->panSweep, voice->panSweep);
        changed |= edit_directsound_adsr(voice);

        if (voice->wav) {
            ImGui::Spacing();
            ImGui::SeparatorText("Sample Info");
            ImGui::Text("Size: %u samples", voice->wav->size);
            ImGui::Text("Frequency: %u Hz", voice->wav->freq);
            ImGui::Text("Loop: %s (start: %u)",
                        (voice->wav->status & 0x4000) ? "Yes" : "No",
                        voice->wav->loopStart);
        }
    } else if (baseType == 0x01) {
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        int sweep = voice->panSweep;
        if (ImGui::SliderInt("Sweep", &sweep, 0, 127)) { voice->panSweep = (uint8_t)sweep; changed = true; }
        int duty = (int)(uintptr_t)voice->wavePointer & 0x03;
        const char *dutyNames[] = { "12.5%", "25%", "50%", "75%" };
        if (ImGui::Combo("Duty Cycle", &duty, dutyNames, 4)) {
            voice->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
            changed = true;
        }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == 0x02) {
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        int duty = (int)(uintptr_t)voice->wavePointer & 0x03;
        const char *dutyNames[] = { "12.5%", "25%", "50%", "75%" };
        if (ImGui::Combo("Duty Cycle", &duty, dutyNames, 4)) {
            voice->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
            changed = true;
        }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == 0x03) {
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == 0x04) {
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        int period = (int)(uintptr_t)voice->wavePointer & 0x01;
        const char *periodNames[] = { "Normal (15-bit)", "Metallic (7-bit)" };
        if (ImGui::Combo("Period", &period, periodNames, 2)) {
            voice->wavePointer = (uint32_t *)(uintptr_t)(period & 0x01);
            changed = true;
        }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == VOICE_CRY || baseType == VOICE_CRY_REVERSE) {
        ImGui::Text("Key: %d", voice->key);
        ImGui::Text("Attack: %d  Decay: %d  Sustain: %d  Release: %d",
                    voice->attack, voice->decay, voice->sustain, voice->release);
        ImGui::TextDisabled("(Cry voices are read-only)");
    } else if (baseType == VOICE_KEYSPLIT) {
        ImGui::TextDisabled("(Keysplit voice - sub-voice editing not supported)");
    } else if (baseType == VOICE_KEYSPLIT_ALL) {
        ImGui::TextDisabled("(Drum Kit voice - sub-voice editing not supported)");
    } else {
        ImGui::TextDisabled("(Unknown voice type)");
    }

    if (changed) {
        app->voiceOverrides[idx] = true;
        m4a_engine_refresh_voices(&app->engine);
    }

    if (app->voiceOverrides[idx]) {
        ImGui::Spacing();
        if (ImGui::Button("Restore Original")) {
            app->loadedVg->voices[idx] = app->originalVoices[idx];
            app->voiceOverrides[idx] = false;
            m4a_engine_refresh_voices(&app->engine);
        }
    }
}

static void render_ui(PlayerApp *app)
{
    ImGui::Begin("poryaaaa Player");
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("General")) {
            render_general_tab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Player")) {
            render_player_tab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Voices")) {
            render_voices_tab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void glfw_error_callback(int error, const char *description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int argc, char **argv)
{
    PlayerApp app = {};
    char startupMidiPath[512] = {0};
    app.selectedVoice = 0;
    app.settings.songMasterVolume = 127;
    app.settings.maxPcmChannels = 5;
    app.sampleRate = 44100.0;
    app.tailSeconds = 3.0;

    const char *startupMidi = NULL;
    resolve_config_path(app.configPath, sizeof(app.configPath), argc > 0 ? argv[0] : NULL);
    if (!app.configPath[0])
        snprintf(app.configPath, sizeof(app.configPath), "%s", "poryaaaa.cfg");
    load_config_file(&app, app.configPath, startupMidiPath, sizeof(startupMidiPath));
    if (startupMidiPath[0])
        startupMidi = startupMidiPath;

    int argi = 1;
    if (argi + 1 < argc && argv[argi][0] != '-' && argv[argi + 1][0] != '-') {
        snprintf(app.settings.projectRoot, sizeof(app.settings.projectRoot), "%s", argv[argi++]);
        snprintf(app.settings.voicegroupName, sizeof(app.settings.voicegroupName), "%s", argv[argi++]);
    }

    for (; argi < argc; argi++) {
        if (strcmp(argv[argi], "--midi") == 0 && argi + 1 < argc) {
            startupMidi = argv[++argi];
            snprintf(app.configMidiPath, sizeof(app.configMidiPath), "%s", startupMidi);
        } else if (strcmp(argv[argi], "--song-volume") == 0 && argi + 1 < argc) {
            int v = atoi(argv[++argi]);
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            app.settings.songMasterVolume = (uint8_t)v;
        } else if (strcmp(argv[argi], "--reverb") == 0 && argi + 1 < argc) {
            int v = atoi(argv[++argi]);
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            app.settings.reverbAmount = (uint8_t)v;
        } else if (strcmp(argv[argi], "--analog-filter") == 0) {
            app.settings.analogFilter = true;
        } else if (strcmp(argv[argi], "--polyphony") == 0 && argi + 1 < argc) {
            int v = atoi(argv[++argi]);
            if (v < 1) v = 1;
            if (v > MAX_PCM_CHANNELS) v = MAX_PCM_CHANNELS;
            app.settings.maxPcmChannels = (uint8_t)v;
        } else if (strcmp(argv[argi], "--sample-rate") == 0 && argi + 1 < argc) {
            int v = atoi(argv[++argi]);
            if (v < 8000) v = 8000;
            app.sampleRate = (double)v;
        } else if (strcmp(argv[argi], "--tail") == 0 && argi + 1 < argc) {
            app.tailSeconds = atof(argv[++argi]);
            if (app.tailSeconds < 0.0) app.tailSeconds = 0.0;
        } else if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argi]);
            print_usage(argv[0]);
            return 1;
        }
    }

    sync_input_buffers(&app);
    if (startupMidi)
        snprintf(app.midiPathBuf, sizeof(app.midiPathBuf), "%s", startupMidi);
    refresh_voicegroup_choices(&app, app.settings.projectRoot);

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

#if defined(__APPLE__)
    const char *glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char *glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow *window = glfwCreateWindow(960, 720, "poryaaaa Player", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    {
        std::lock_guard<std::mutex> lock(app.mutex);
        init_or_reset_engine_locked(&app);
    }

#ifdef __linux__
    suppress_alsa_errors();
    ma_backend linuxBackends[] = { ma_backend_pulseaudio, ma_backend_alsa };
    ma_context context;
    bool hasContext = (ma_context_init(linuxBackends, 2, NULL, &context) == MA_SUCCESS);
#endif

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = (ma_uint32)app.sampleRate;
    cfg.dataCallback = audio_callback;
    cfg.pUserData = &app;

#ifdef __linux__
    ma_result initResult = ma_device_init(hasContext ? &context : NULL, &cfg, &app.device);
#else
    ma_result initResult = ma_device_init(NULL, &cfg, &app.device);
#endif
    if (initResult != MA_SUCCESS) {
        fprintf(stderr, "Failed to initialize audio playback device\n");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
#ifdef __linux__
        if (hasContext)
            ma_context_uninit(&context);
#endif
        return 1;
    }
    app.deviceInitialized = true;

    if (ma_device_start(&app.device) != MA_SUCCESS) {
        fprintf(stderr, "Failed to start audio playback device\n");
        ma_device_uninit(&app.device);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
#ifdef __linux__
        if (hasContext)
            ma_context_uninit(&context);
#endif
        return 1;
    }

    if (app.settings.projectRoot[0] && app.settings.voicegroupName[0])
        reload_voicegroup(&app);
    if (startupMidi)
        load_midi(&app, startupMidi);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        render_ui(&app);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (app.deviceInitialized)
        ma_device_uninit(&app.device);
#ifdef __linux__
    if (hasContext)
        ma_context_uninit(&context);
#endif
    if (app.loadedVg)
        voicegroup_free(app.loadedVg);
    midi_timeline_free(app.timeline);
    voicegroup_name_list_free(app.voicegroupChoices);
    if (app.engineInitialized)
        m4a_engine_destroy(&app.engine);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
