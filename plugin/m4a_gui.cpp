/*
 * m4a_gui.cpp - Dear ImGui + Pugl GUI for the M4A plugin.
 *
 * Provides a simple settings panel where the user can change the project
 * root, voicegroup, reverb, and volume levels in real time from the DAW.
 *
 * Thread-safety: all functions must be called from the main thread.
 */

#include <pugl/pugl.h>

#ifdef __APPLE__
#include "metal/pugl_mac_metal.h"
#include "imgui_impl_metal.h"
#else
#include <pugl/gl.h>
#include "imgui_impl_opengl3.h"
#endif

#include "imgui.h"
#include "imgui_impl_pugl.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/* Timer ID for the internal render timer 
    Bitwig does not give the plugin a timer, 
    so we use a timer from Pugl to drive GUI updates */
static const uintptr_t RENDER_TIMER_ID = 1;

/* ---- Debug logging ---- */
static const char *s_logPath = nullptr;

static void gui_log(const char *fmt, ...)
{
    if (!s_logPath) return;
    FILE *f = fopen(s_logPath, "a");
    if (!f) return;
    time_t t = time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&t));
    fprintf(f, "[%s] ", tbuf);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Our C interface */
#include "m4a_gui.h"
#include "m4a_engine.h"
#include "m4a_plugin.h"
#include "m4a_engine_recorder.h"

/* CLAP GUI extension (for notifying host when floating window closes) */
#include <clap/ext/gui.h>

/* ---- Constants ---- */

static const int GUI_W = 540;
static const int GUI_H = 500;

/* ---- GUI state ---- */

struct M4AGuiState {
    PuglWorld     *world;
    PuglView      *view;
    ImGuiContext  *imguiCtx;
    const clap_host_t *host;

    bool           realized;   /* true after puglRealize succeeds */
    bool           renderInited; /* true after render backend init */

    /* Cached size from PUGL_CONFIGURE */
    uint32_t       cachedWidth;
    uint32_t       cachedHeight;

    /* Currently displayed settings */
    M4AGuiSettings settings;

    /* Editable text buffers (not applied until "Reload" is clicked) */
    char projectRootBuf[512];
    char voicegroupBuf[256];

    /* Pending change flags (cleared by poll_changes) */
    bool settingsChanged;
    bool reloadRequested;
    double midiActivityUntil[16];
    double xcmdActivityUntil;
    double validXcmdUntil;
    char latestXcmd[128];

    /* True after set_parent() — host drives sizing and visibility */
    bool isEmbedded;

    /* True after the user closes the floating window */
    bool wasClosed;

    /* True when the internal pugl render timer is active */

    M4AGuiTimerCallback internalTimerCallback;
    void *internalTimerUserData;

    /* Voice editor state */
    ToneData *liveVoices;
    const ToneData *originalVoices;
    bool *voiceOverrides;
    int selectedVoice;
    int pendingRestoreVoice;  /* -1 = none */
    bool voicesDirty;         /* set when any voice param is edited */

    /* Sample swapper state */
    const ProjectAssetEntry *directsoundAssets;
    int directsoundAssetCount;
    const ProjectAssetEntry *progWaveAssets;
    int progWaveAssetCount;
    const ProjectAssetOverride *assetOverrides;
    char sampleFilterText[256];

    bool pendingSampleSwap;
    int pendingSwapVoice;
    ProjectAssetKind pendingSwapKind;
    char pendingSwapFileName[256];

    /* Recorder tab: direct pointer to plugin data (M4APluginData*) for
     * armed flag and path. Stored as void* to avoid circular include in header. */
    void *plugin_data;
    /* Status message shown after a save attempt */
    char recorderStatus[256];

    /* Audio device callbacks (standalone wires these; DAW build leaves NULL).
     * Cached device list is repopulated on first render and on Refresh so we
     * don't re-enumerate CoreAudio every frame. */
    M4AHostAudioApi audioApi;
    bool            audioDevicesDirty;
    int             audioDeviceCount;
    M4AAudioDeviceInfo audioDevices[32];
};

/* ---- Internal helpers ---- */

static void sync_buffers(M4AGuiState *gui)
{
    snprintf(gui->projectRootBuf, sizeof(gui->projectRootBuf),
             "%s", gui->settings.projectRoot);
    snprintf(gui->voicegroupBuf, sizeof(gui->voicegroupBuf),
             "%s", gui->settings.voicegroupName);
}

/* ---- Voice type helpers ---- */

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

/* Edit ADSR for DirectSound voices (0-255 range). Returns true if changed. */
static bool edit_directsound_adsr(ToneData *voice)
{
    bool changed = false;
    int a = voice->attack, d = voice->decay, s = voice->sustain, r = voice->release;
    if (ImGui::SliderInt("Attack##ds", &a, 0, 255))  { voice->attack  = (uint8_t)a; changed = true; }
    if (ImGui::SliderInt("Decay##ds",  &d, 0, 255))  { voice->decay   = (uint8_t)d; changed = true; }
    if (ImGui::SliderInt("Sustain##ds",&s, 0, 255))   { voice->sustain = (uint8_t)s; changed = true; }
    if (ImGui::SliderInt("Release##ds",&r, 0, 255))   { voice->release = (uint8_t)r; changed = true; }
    return changed;
}

/* Edit ADSR for CGB voices (limited range). Returns true if changed. */
static bool edit_cgb_adsr(ToneData *voice)
{
    bool changed = false;
    int a = voice->attack, d = voice->decay, s = voice->sustain, r = voice->release;
    if (ImGui::SliderInt("Attack##cgb", &a, 0, 7))   { voice->attack  = (uint8_t)a; changed = true; }
    if (ImGui::SliderInt("Decay##cgb",  &d, 0, 7))   { voice->decay   = (uint8_t)d; changed = true; }
    if (ImGui::SliderInt("Sustain##cgb",&s, 0, 15))   { voice->sustain = (uint8_t)s; changed = true; }
    if (ImGui::SliderInt("Release##cgb",&r, 0, 7))    { voice->release = (uint8_t)r; changed = true; }
    return changed;
}

/* ---- Tab rendering ---- */

static void render_general_tab(M4AGuiState *gui)
{
    double now = ImGui::GetTime();
    bool xcmdActive = now < gui->xcmdActivityUntil;
    bool validXcmdActive = now < gui->validXcmdUntil;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    auto draw_led = [&](bool active, const ImVec4 &onColor, const ImVec4 &offColor, const char *label) {
        ImVec2 ledCenter = ImGui::GetCursorScreenPos();
        ledCenter.x += 7.0f;
        ledCenter.y += 9.0f;
        drawList->AddCircleFilled(ledCenter, 5.0f,
                                  ImGui::GetColorU32(active ? onColor : offColor));
        ImGui::Dummy(ImVec2(14.0f, 18.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    };

    /* Per-channel MIDI activity: 16 compact LEDs in a row, one per MIDI
     * channel. Each LED pulses independently when its channel sees an event. */
    static const ImVec4 kChanOn  = ImVec4(0.18f, 0.95f, 0.35f, 1.0f);
    static const ImVec4 kChanOff = ImVec4(0.18f, 0.24f, 0.20f, 1.0f);
    ImGui::TextUnformatted("MIDI Activity");
    {
        const float ledSlot = 18.0f;
        ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
        for (int ch = 0; ch < 16; ch++) {
            bool active = now < gui->midiActivityUntil[ch];
            ImVec2 c;
            c.x = rowOrigin.x + ch * ledSlot + 7.0f;
            c.y = rowOrigin.y + 9.0f;
            drawList->AddCircleFilled(c, 5.0f,
                                       ImGui::GetColorU32(active ? kChanOn : kChanOff));
        }
        ImGui::Dummy(ImVec2(ledSlot * 16.0f, 18.0f));

        /* Channel-number labels under each LED (1-based to match how DAWs
         * present MIDI channels). */
        ImVec2 labelOrigin = ImGui::GetCursorScreenPos();
        for (int ch = 0; ch < 16; ch++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%d", ch + 1);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            ImVec2 p;
            p.x = labelOrigin.x + ch * ledSlot + (14.0f - sz.x) * 0.5f;
            p.y = labelOrigin.y;
            drawList->AddText(p, ImGui::GetColorU32(ImGuiCol_TextDisabled), buf);
        }
        ImGui::Dummy(ImVec2(ledSlot * 16.0f, ImGui::GetTextLineHeight()));
    }
    ImGui::Spacing();

    draw_led(xcmdActive,
             ImVec4(0.95f, 0.75f, 0.18f, 1.0f),
             ImVec4(0.26f, 0.22f, 0.14f, 1.0f),
             "XCMD Traffic");
    draw_led(validXcmdActive,
             ImVec4(0.18f, 0.75f, 0.95f, 1.0f),
             ImVec4(0.15f, 0.22f, 0.28f, 1.0f),
             "Valid XCMD");
    ImGui::Text("XCMD Target: %s", gui->latestXcmd[0] ? gui->latestXcmd : "None");
    ImGui::Spacing();

    /* ---- Project Settings ---- */
    ImGui::SeparatorText("Project Settings");

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Project Root:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##root", gui->projectRootBuf, sizeof(gui->projectRootBuf));

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Voicegroup:  ");
    ImGui::SameLine();
    {
        float btnW = 80.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - spacing);
    }
    ImGui::InputText("##vg", gui->voicegroupBuf, sizeof(gui->voicegroupBuf));
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(80, 0))) {
        snprintf(gui->settings.projectRoot,    sizeof(gui->settings.projectRoot),
                 "%s", gui->projectRootBuf);
        snprintf(gui->settings.voicegroupName, sizeof(gui->settings.voicegroupName),
                 "%s", gui->voicegroupBuf);
        gui->settingsChanged = true;
        gui->reloadRequested = true;
    }

    /* Voicegroup load status */
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Status:      ");
    ImGui::SameLine();
    if (gui->settings.voicegroupLoaded)
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Voicegroup loaded");
    else
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "Voicegroup not loaded");

    ImGui::Spacing();

    /* ---- Audio Settings ---- */
    ImGui::SeparatorText("Audio Settings");
    {
        int v = (int)gui->settings.songMasterVolume;
        if (ImGui::SliderInt("Song Volume (0-127)", &v, 0, 127)) {
            gui->settings.songMasterVolume = (uint8_t)v;
            gui->settingsChanged = true;
        }
    }
    {
        int v = (int)gui->settings.reverbAmount;
        if (ImGui::SliderInt("Reverb (0-127)", &v, 0, 127)) {
            gui->settings.reverbAmount = (uint8_t)v;
            gui->settingsChanged = true;
        }
    }
    {
        int v = (int)gui->settings.maxPcmChannels;
        if (ImGui::SliderInt("Polyphony (1-12)", &v, 1, MAX_PCM_CHANNELS)) {
            gui->settings.maxPcmChannels = (uint8_t)v;
            gui->settingsChanged = true;
        }
    }
    if (ImGui::Checkbox("GBA Analog Filter", &gui->settings.analogFilter))
        gui->settingsChanged = true;
    if (ImGui::Checkbox("Mono Output", &gui->settings.monoOutput))
        gui->settingsChanged = true;

    /* ---- Output Device (standalone only) ----
     * audioApi.list_outputs is wired by the standalone entry to RtAudio.
     * In a DAW-hosted CLAP build the host owns audio routing, so the
     * callbacks stay NULL and this whole control is hidden. */
    if (gui->audioApi.list_outputs && gui->audioApi.set_output) {
        if (gui->audioDevicesDirty) {
            gui->audioDeviceCount = gui->audioApi.list_outputs(
                gui->audioApi.ctx, gui->audioDevices,
                (int)(sizeof(gui->audioDevices) / sizeof(gui->audioDevices[0])));
            gui->audioDevicesDirty = false;
        }
        unsigned int currentID = gui->audioApi.current_id
            ? gui->audioApi.current_id(gui->audioApi.ctx) : 0u;
        const char *currentName = "(unknown)";
        for (int i = 0; i < gui->audioDeviceCount; i++) {
            if (gui->audioDevices[i].id == currentID) {
                currentName = gui->audioDevices[i].name;
                break;
            }
        }
        if (ImGui::BeginCombo("Output Device", currentName)) {
            for (int i = 0; i < gui->audioDeviceCount; i++) {
                bool selected = (gui->audioDevices[i].id == currentID);
                if (ImGui::Selectable(gui->audioDevices[i].name, selected)) {
                    gui->audioApi.set_output(gui->audioApi.ctx,
                                              gui->audioDevices[i].id);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##audiodev"))
            gui->audioDevicesDirty = true;
    }
}

/* Portable case-insensitive substring search (strcasestr is not on Windows). */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!needle[0]) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

/*
 * Render a filterable sample selector combo for a voice slot.
 */
static void render_sample_selector(M4AGuiState *gui, int voiceIndex,
                                   const char *label, ProjectAssetKind kind,
                                   const ProjectAssetEntry *assets, int assetCount)
{
    if (!assets || assetCount <= 0) return;

    ImGui::Spacing();
    ImGui::SeparatorText(label);

    /* Determine preview text: current override name, or placeholder */
    const char *preview = "(select sample...)";
    const char *currentName = NULL;
    if (gui->assetOverrides && gui->assetOverrides[voiceIndex].active) {
        currentName = gui->assetOverrides[voiceIndex].fileName;
        preview = currentName;
    }

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##sampleCombo", preview)) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##filter", "Filter...", gui->sampleFilterText, sizeof(gui->sampleFilterText));

        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere(-1);

        ImGui::Separator();

        for (int i = 0; i < assetCount; i++) {
            if (gui->sampleFilterText[0] != '\0' &&
                ci_strstr(assets[i].fileName, gui->sampleFilterText) == NULL)
                continue;

            bool isSelected = (currentName && strcmp(assets[i].fileName, currentName) == 0);
            if (ImGui::Selectable(assets[i].fileName, isSelected)) {
                gui->pendingSampleSwap = true;
                gui->pendingSwapVoice = voiceIndex;
                gui->pendingSwapKind = kind;
                strncpy(gui->pendingSwapFileName, assets[i].fileName,
                        sizeof(gui->pendingSwapFileName) - 1);
                gui->pendingSwapFileName[sizeof(gui->pendingSwapFileName) - 1] = '\0';
                ImGui::CloseCurrentPopup();
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }

        ImGui::EndCombo();
    }
}

static void render_voices_tab(M4AGuiState *gui)
{
    if (!gui->liveVoices) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "No voicegroup loaded");
        return;
    }

    /* Voice selector */
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::SliderInt("##voiceSlider", &gui->selectedVoice, 0, 127);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##voiceInput", &gui->selectedVoice, 1, 10);
    if (gui->selectedVoice < 0) gui->selectedVoice = 0;
    if (gui->selectedVoice > 127) gui->selectedVoice = 127;

    int idx = gui->selectedVoice;
    ToneData *voice = &gui->liveVoices[idx];
    uint8_t type = voice->type;

    /* Type label */
    ImGui::Text("Type: %s (0x%02X)", voice_type_name(type), type);
    if (type == VOICE_DIRECTSOUND_NO_RESAMPLE) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[Fixed]");
    }

    /* Modified indicator */
    if (gui->voiceOverrides[idx]) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "(modified)");
    }

    ImGui::Separator();

    bool changed = false;
    uint8_t baseType = type & ~VOICE_TYPE_FIX;

    /* Dispatch to per-type editor */
    if (baseType == 0x00) {
        /* DirectSound — key and panSweep are metadata only */
        ImGui::Text("Key: %d", voice->key);
        ImGui::Text("Pan/Sweep: %d (0x%02X)", voice->panSweep, voice->panSweep);
        changed |= edit_directsound_adsr(voice);

        /* Read-only sample info */
        if (voice->wav) {
            ImGui::Spacing();
            ImGui::SeparatorText("Sample Info");
            ImGui::Text("Size: %u samples", voice->wav->size);
            ImGui::Text("Frequency: %u Hz", voice->wav->freq);
            ImGui::Text("Loop: %s (start: %u)", (voice->wav->status & 0x4000) ? "Yes" : "No", voice->wav->loopStart);
        }

        render_sample_selector(gui, idx, "Sample", PROJECT_ASSET_DIRECTSOUND,
                               gui->directsoundAssets, gui->directsoundAssetCount);
    } else if (baseType == 0x01) {
        /* Square 1 */
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
        /* Square 2 */
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
        /* Programmable Wave */
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        changed |= edit_cgb_adsr(voice);

        render_sample_selector(gui, idx, "Wave", PROJECT_ASSET_PROG_WAVE,
                               gui->progWaveAssets, gui->progWaveAssetCount);
    } else if (baseType == 0x04) {
        /* Noise */
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
        /* Cry — read-only display */
        ImGui::Text("Key: %d", voice->key);
        ImGui::Text("Attack: %d  Decay: %d  Sustain: %d  Release: %d",
                     voice->attack, voice->decay, voice->sustain, voice->release);
        ImGui::TextDisabled("(Cry voices are read-only)");
    } else if (baseType == VOICE_KEYSPLIT) {
        ImGui::TextDisabled("(Keysplit voice — sub-voice editing not supported)");
    } else if (baseType == VOICE_KEYSPLIT_ALL) {
        ImGui::TextDisabled("(Drum Kit voice — sub-voice editing not supported)");
    } else {
        ImGui::TextDisabled("(Unknown voice type)");
    }

    if (changed) {
        gui->voiceOverrides[idx] = true;
        gui->voicesDirty = true;
    }

    /* Restore button */
    if (gui->voiceOverrides[idx]) {
        ImGui::Spacing();
        if (ImGui::Button("Restore Original")) {
            gui->pendingRestoreVoice = idx;
        }
    }
}

static void render_recorder_tab(M4AGuiState *gui)
{
    M4APluginData *data = static_cast<M4APluginData *>(gui->plugin_data);
    if (!data) {
        ImGui::TextUnformatted("Recorder UI wiring deferred — engine ptr plumbing");
        return;
    }

    /* Record toggle — only when this is on does the audio thread push MIDI
     * events into the recorder buffer. */
    bool armed = atomic_load(&data->recorderArmed);
    if (ImGui::Checkbox("Record", &armed))
        atomic_store(&data->recorderArmed, armed);

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m4a_engine_recorder_reset(&data->engine);
        atomic_store(&data->recorderSeenPC,  0u);
        atomic_store(&data->recorderSeenVol, 0u);
        atomic_store(&data->recorderSeenPan, 0u);
        gui->recorderStatus[0] = '\0';
    }

    /* Status counter */
    uint64_t evCount = m4a_engine_recorder_event_count(&data->engine);
    ImGui::Text("Buffered: %llu events", (unsigned long long)evCount);

    /* Filename input */
    ImGui::InputText("Filename", data->recorderPath, sizeof(data->recorderPath));

    /* Save button */
    if (ImGui::Button("Save SMF")) {
        bool ok = m4a_engine_recorder_save_smf(&data->engine, data->recorderPath, 480, 120.0);
        snprintf(gui->recorderStatus, sizeof(gui->recorderStatus),
                 ok ? "Saved: %s" : "Failed: %s", data->recorderPath);
    }

    if (gui->recorderStatus[0])
        ImGui::TextUnformatted(gui->recorderStatus);

    /* Per-channel capture indicators: 12 rows, three LEDs each.
     * A LED lights green once the recorder has captured an event of that
     * type for that channel since the last Clear. */
    ImGui::Separator();
    uint32_t pcMask  = atomic_load(&data->recorderSeenPC);
    uint32_t volMask = atomic_load(&data->recorderSeenVol);
    uint32_t panMask = atomic_load(&data->recorderSeenPan);
    auto led = [](uint32_t mask, int ch) {
        bool on = (mask >> ch) & 1u;
        ImGui::TextColored(on ? ImVec4(0.18f, 0.95f, 0.35f, 1.0f)
                              : ImVec4(0.18f, 0.24f, 0.20f, 1.0f), "\xE2\x97\x8F");
    };
    if (ImGui::BeginTable("rec_indicators", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("PC");
        ImGui::TableSetupColumn("Vol");
        ImGui::TableSetupColumn("Pan");
        ImGui::TableHeadersRow();
        for (int ch = 0; ch < 12; ch++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", ch + 1);
            ImGui::TableSetColumnIndex(1); led(pcMask,  ch);
            ImGui::TableSetColumnIndex(2); led(volMask, ch);
            ImGui::TableSetColumnIndex(3); led(panMask, ch);
        }
        ImGui::EndTable();
    }
}

/* Render a single ImGui frame — called from PUGL_EXPOSE. */
static void render_frame(M4AGuiState *gui)
{
    ImGui::SetCurrentContext(gui->imguiCtx);

#ifdef __APPLE__
    PuglMetalContext *mtlCtx = (PuglMetalContext *)puglGetContext(gui->view);
    ImGui_ImplMetal_NewFrame(mtlCtx->renderPassDescriptor);
#else
    ImGui_ImplOpenGL3_NewFrame();
#endif
    ImGui_ImplPugl_NewFrame();
    ImGui::NewFrame();

    uint32_t fbW = gui->cachedWidth;
    uint32_t fbH = gui->cachedHeight;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)fbW, (float)fbH));

    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar      |
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoCollapse      |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##Main", nullptr, wflags);

    /* ---- Plugin title ---- */
    ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "poryaaaa");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 160.0f);
    ImGui::TextDisabled("pokeemerald");
    ImGui::Separator();
    ImGui::Spacing();

    /* ---- Tabbed content ---- */
    if (ImGui::BeginTabBar("##Tabs")) {
        if (ImGui::BeginTabItem("General")) {
            render_general_tab(gui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Voices")) {
            render_voices_tab(gui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Recorder")) {
            render_recorder_tab(gui);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    /* ---- Render ---- */
    ImGui::Render();
#ifdef __APPLE__
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),
                                   mtlCtx->commandBuffer,
                                   mtlCtx->renderEncoder);
#else
    glViewport(0, 0, (int)fbW, (int)fbH);
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
    /* Pugl handles buffer swap / command buffer commit */
}

/* ---- Pugl event handler ---- */

static PuglStatus pugl_event_handler(PuglView *view, const PuglEvent *event)
{
    M4AGuiState *gui = (M4AGuiState *)puglGetHandle(view);
    if (!gui)
        return PUGL_SUCCESS;

    ImGui::SetCurrentContext(gui->imguiCtx);

    switch (event->type)
    {
    case PUGL_REALIZE:
        /* Graphics context is now current — initialize render backend */
        if (!gui->renderInited) {
#ifdef __APPLE__
        {
            PuglMetalContext *ctx = (PuglMetalContext *)puglGetContext(gui->view);
            ImGui_ImplMetal_Init(ctx->device);
        }
#else
            ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
            gui->renderInited = true;
        }
        break;

    case PUGL_UNREALIZE:
        /* Render context is current — shut down render backend if still active.
         * NOTE: puglFreeView() on Windows does NOT dispatch PUGL_UNREALIZE,
         * so the explicit shutdown in m4a_gui_destroy() handles the normal
         * teardown path.  This case handles any other unrealize scenario. */
        if (gui->renderInited) {
#ifdef __APPLE__
            ImGui_ImplMetal_Shutdown();
#else
            ImGui_ImplOpenGL3_Shutdown();
#endif
            gui->renderInited = false;
        }
        break;

    case PUGL_CONFIGURE:
        gui->cachedWidth  = event->configure.width;
        gui->cachedHeight = event->configure.height;
        break;

    case PUGL_UPDATE:
        /* Request a redraw on every update so we render continuously */
        puglObscureView(view);
        break;

    case PUGL_EXPOSE:
        if (gui->renderInited)
            render_frame(gui);
        break;
    case PUGL_TIMER:
        if (event->timer.id == RENDER_TIMER_ID) {
            if (gui->internalTimerCallback)
            // This is currently just an alias to m4a_plugin.c -> timer_on_timer((const clap_plugin_t *)user_data, 0);
                gui->internalTimerCallback(gui->internalTimerUserData);
            else {
                /* This would happen if the host provided a timer *but* somehow 
                another Pugl timer started (i dont know how or why or if thats possible)*/
                gui_log("More than 1 Pugl timer is running. That's odd...");
                m4a_gui_tick(gui);
                }
            }
            break;

    case PUGL_CLOSE:
        gui->wasClosed = true;
        m4a_gui_stop_internal_timer(gui);
        // gui_log("pugl_event_handler: PUGL_CLOSE");
        if (gui->host) {
            const clap_host_gui_t *hostGui =
                (const clap_host_gui_t *)gui->host->get_extension(gui->host, CLAP_EXT_GUI);
            if (hostGui)
                hostGui->closed(gui->host, false /* was_destroyed */);
        }
        break;

    case PUGL_BUTTON_PRESS:
        /* Claim keyboard focus so that subsequent key/text events are routed
         * to our child window.  In embedded mode the host's message pump does
         * not automatically give the child focus on click. */
        puglGrabFocus(view);
        ImGui_ImplPugl_ProcessEvent(event);
        break;

    case PUGL_BUTTON_RELEASE:
        ImGui_ImplPugl_ProcessEvent(event);
        break;

    case PUGL_KEY_PRESS:
    case PUGL_KEY_RELEASE:
    {
        ImGuiIO &io = ImGui::GetIO();

        /* Tells Pugl to not handle spacebar presses *unless* ImGui wants text input
        See third_party/pugl/mac.m > key_down handler for more details */
        const PuglMods mods = event->key.state;
        const bool plainSpace =
            event->key.key == PUGL_KEY_SPACE &&
            (mods & (PUGL_MOD_SHIFT | PUGL_MOD_CTRL | PUGL_MOD_ALT | PUGL_MOD_SUPER)) == 0;

        if (gui->isEmbedded && plainSpace && !io.WantTextInput) // in case ur focusing text input
            return PUGL_UNSUPPORTED;

        ImGui_ImplPugl_ProcessEvent(event);
        break;
    }

    default:
        /* Forward all other input events to ImGui */
        ImGui_ImplPugl_ProcessEvent(event);
        break;
    }

    return PUGL_SUCCESS;
}

/* ---- Public C interface ---- */

extern "C" {

M4AGuiState *m4a_gui_create(const clap_host_t *host, const M4AGuiSettings *initial,
                             const char *log_path)
{
    s_logPath = log_path;
    // gui_log("m4a_gui_create: begin");

    M4AGuiState *gui = new M4AGuiState();
    memset(gui, 0, sizeof(*gui));
    gui->host         = host;
    gui->cachedWidth  = (uint32_t)GUI_W;
    gui->cachedHeight = (uint32_t)GUI_H;
    gui->selectedVoice       = 0;
    gui->pendingRestoreVoice = -1;

    if (initial) {
        gui->settings = *initial;
    } else {
        memset(&gui->settings, 0, sizeof(gui->settings));
        gui->settings.masterVolume     = 15;
        gui->settings.songMasterVolume = 127;
        gui->settings.monoOutput       = false;
    }
    sync_buffers(gui);

    /* Create Pugl world and view */
    gui->world = puglNewWorld(PUGL_MODULE, 0);
    if (!gui->world) {
        gui_log("m4a_gui_create: puglNewWorld failed");
        delete gui;
        return nullptr;
    }
    puglSetWorldString(gui->world, PUGL_CLASS_NAME, "poryaaaa");

    gui->view = puglNewView(gui->world);
    if (!gui->view) {
        gui_log("m4a_gui_create: puglNewView failed");
        puglFreeWorld(gui->world);
        delete gui;
        return nullptr;
    }

    /* Configure the view */
#ifdef __APPLE__
    puglSetBackend(gui->view, puglMetalBackend());
#else
    puglSetBackend(gui->view, puglGlBackend());
    puglSetViewHint(gui->view, PUGL_CONTEXT_API,           PUGL_OPENGL_API);
    puglSetViewHint(gui->view, PUGL_CONTEXT_VERSION_MAJOR, 3);
    puglSetViewHint(gui->view, PUGL_CONTEXT_VERSION_MINOR, 3);
    puglSetViewHint(gui->view, PUGL_CONTEXT_PROFILE,       PUGL_OPENGL_CORE_PROFILE);
    puglSetViewHint(gui->view, PUGL_DOUBLE_BUFFER,         1);
#endif
    puglSetViewHint(gui->view, PUGL_RESIZABLE,             1);
    puglSetSizeHint(gui->view, PUGL_DEFAULT_SIZE, (PuglSpan)GUI_W, (PuglSpan)GUI_H);
    puglSetSizeHint(gui->view, PUGL_MIN_SIZE,     (PuglSpan)200,   (PuglSpan)150);
    puglSetViewString(gui->view, PUGL_WINDOW_TITLE, "poryaaaa");

    puglSetHandle(gui->view, gui);
    puglSetEventFunc(gui->view, pugl_event_handler);

    /* Create ImGui context per instance */
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale = 1.2f;

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding    = ImVec2(12, 12);
    style.ItemSpacing      = ImVec2(8, 6);
    style.FramePadding     = ImVec2(6, 4);
    style.GrabMinSize      = 10.0f;
    style.WindowRounding   = 4.0f;
    style.FrameRounding    = 3.0f;
    style.GrabRounding     = 3.0f;

    ImGui_ImplPugl_Init(gui->view);

    gui->imguiCtx = ctx;

    /* Do NOT realize yet — that happens in set_parent() or show() */
    gui_log("m4a_gui_create: success");
    return gui;
}

void m4a_gui_destroy(M4AGuiState *gui)
{
    if (!gui)
        return;


    /* Stop the internal render timer before tearing down GL/ImGui */
    m4a_gui_stop_internal_timer(gui);

    ImGui::SetCurrentContext(gui->imguiCtx);

    /* puglFreeView() on Windows calls puglFreeViewInternals() which destroys
     * the GL context WITHOUT dispatching PUGL_UNREALIZE first.  Explicitly
     * enter the context and shut down the render backend before the view
     * (and its context) are freed. */
    if (gui->view && gui->renderInited) {
        puglEnterContext(gui->view);
#ifdef __APPLE__
        ImGui_ImplMetal_Shutdown();
#else
        ImGui_ImplOpenGL3_Shutdown();
#endif
        gui->renderInited = false;
        puglLeaveContext(gui->view);
    }

    ImGui_ImplPugl_Shutdown();

    if (gui->view) {
        puglFreeView(gui->view);
        gui->view = nullptr;
    }

    ImGui::DestroyContext(gui->imguiCtx);
    gui->imguiCtx = nullptr;

    if (gui->world) {
        puglFreeWorld(gui->world);
        gui->world = nullptr;
    }

    delete gui;
    gui_log("m4a_gui_destroy: done");
}

bool m4a_gui_set_parent(M4AGuiState *gui, uintptr_t native_parent)
{
    gui_log("m4a_gui_set_parent: parent=0x%zx", (size_t)native_parent);
    if (!gui || !gui->view) return false;
    if (gui->realized) {
        gui_log("m4a_gui_set_parent: already realized");
        return false;
    }

    puglSetParent(gui->view, (PuglNativeView)native_parent);

    PuglStatus st = puglRealize(gui->view);
    if (st != PUGL_SUCCESS) {
        gui_log("m4a_gui_set_parent: puglRealize failed (%d)", (int)st);
        return false;
    }
    gui->realized   = true;
    gui->isEmbedded = true;
    gui_log("m4a_gui_set_parent: success");
    return true;
}

bool m4a_gui_show(M4AGuiState *gui)
{
    gui_log("m4a_gui_show called");
    if (!gui || !gui->view) return false;

    if (!gui->realized) {
        /* Floating mode: realize now (no parent) */
        PuglStatus st = puglRealize(gui->view);
        if (st != PUGL_SUCCESS) {
            gui_log("m4a_gui_show: puglRealize failed (%d)", (int)st);
            return false;
        }
        gui->realized = true;
        gui_log("m4a_gui_show: realized as floating");
    }

    /* Embedded views must not manipulate the host's window (orderFront etc.),
     * so use PUGL_SHOW_PASSIVE.  Floating windows should raise normally. */
    puglShow(gui->view, gui->isEmbedded ? PUGL_SHOW_PASSIVE : PUGL_SHOW_RAISE);
    return true;
}

bool m4a_gui_hide(M4AGuiState *gui)
{
    if (!gui || !gui->view) return false;
    m4a_gui_stop_internal_timer(gui);
    puglHide(gui->view);
    return true;
}

void m4a_gui_get_size(M4AGuiState *gui, uint32_t *width, uint32_t *height)
{
    if (!gui) {
        *width  = (uint32_t)GUI_W;
        *height = (uint32_t)GUI_H;
        return;
    }
    /* cachedWidth/Height are in backing pixels (from Pugl CONFIGURE).
     * The CLAP host expects logical pixels (points on macOS), so divide
     * by the backing scale factor. */
    double scale = gui->view ? puglGetScaleFactor(gui->view) : 1.0;
    if (scale < 1.0) scale = 1.0;
    *width  = (uint32_t)(gui->cachedWidth  / scale);
    *height = (uint32_t)(gui->cachedHeight / scale);
}

bool m4a_gui_set_size(M4AGuiState *gui, uint32_t width, uint32_t height)
{
    if (!gui || !gui->view) return false;
    /* The host sends logical pixels; Pugl's PUGL_CURRENT_SIZE expects
     * backing pixels, so scale up. */
    double scale = puglGetScaleFactor(gui->view);
    if (scale < 1.0) scale = 1.0;
    PuglSpan pw = (PuglSpan)(width  * scale);
    PuglSpan ph = (PuglSpan)(height * scale);
    puglSetSizeHint(gui->view, PUGL_CURRENT_SIZE, pw, ph);
    return true;
}

bool m4a_gui_can_resize(M4AGuiState *gui)
{
    return gui && gui->isEmbedded;
}

void m4a_gui_update_settings(M4AGuiState *gui, const M4AGuiSettings *settings)
{
    if (!gui || !settings) return;
    gui->settings = *settings;
    sync_buffers(gui);
}

void m4a_gui_pulse_midi_activity(M4AGuiState *gui, int channel)
{
    if (!gui || !gui->imguiCtx)
        return;
    if (channel < 0 || channel >= 16)
        return;
    ImGui::SetCurrentContext(gui->imguiCtx);
    gui->midiActivityUntil[channel] = ImGui::GetTime() + 0.15;
}

void m4a_gui_pulse_xcmd_activity(M4AGuiState *gui)
{
    if (!gui || !gui->imguiCtx)
        return;
    ImGui::SetCurrentContext(gui->imguiCtx);
    gui->xcmdActivityUntil = ImGui::GetTime() + 0.15;
}

void m4a_gui_pulse_valid_xcmd(M4AGuiState *gui)
{
    if (!gui || !gui->imguiCtx)
        return;
    ImGui::SetCurrentContext(gui->imguiCtx);
    gui->validXcmdUntil = ImGui::GetTime() + 0.15;
}

void m4a_gui_set_latest_xcmd(M4AGuiState *gui, const char *text)
{
    if (!gui)
        return;

    snprintf(gui->latestXcmd, sizeof(gui->latestXcmd), "%s", text ? text : "");
}

bool m4a_gui_poll_changes(M4AGuiState *gui, M4AGuiSettings *out, bool *reload_voicegroup)
{
    if (!gui || !gui->settingsChanged)
        return false;

    *out               = gui->settings;
    *reload_voicegroup = gui->reloadRequested;
    gui->settingsChanged  = false;
    gui->reloadRequested  = false;
    return true;
}

bool m4a_gui_was_closed(M4AGuiState *gui)
{
    return gui && gui->wasClosed;
}

void m4a_gui_tick(M4AGuiState *gui)
{
    if (!gui || !gui->world)
        return;

    /* Schedule a redraw, then process events (non-blocking) */
    if (gui->view && gui->realized)
        puglObscureView(gui->view);

    puglUpdate(gui->world, 0.0);
}

void m4a_gui_set_internal_timer_callback(M4AGuiState *gui,
                                          M4AGuiTimerCallback callback,
                                          void *user_data)
{
    if (!gui)
        return;
    gui->internalTimerCallback = callback;
    gui->internalTimerUserData = user_data;
}

void m4a_gui_set_voice_data(M4AGuiState *gui,
                             ToneData *liveVoices,
                             const ToneData *originalVoices,
                             bool *overrides)
{
    if (!gui) return;
    gui->liveVoices     = liveVoices;
    gui->originalVoices = originalVoices;
    gui->voiceOverrides = overrides;
    if (!liveVoices)
        gui->pendingRestoreVoice = -1;
}

bool m4a_gui_poll_voice_restore(M4AGuiState *gui, int *voiceIndex)
{
    if (!gui || gui->pendingRestoreVoice < 0)
        return false;
    *voiceIndex = gui->pendingRestoreVoice;
    gui->pendingRestoreVoice = -1;
    return true;
}

bool m4a_gui_poll_voices_dirty(M4AGuiState *gui)
{
    if (!gui || !gui->voicesDirty)
        return false;
    gui->voicesDirty = false;
    return true;
}

void m4a_gui_start_internal_timer(M4AGuiState *gui)
{
    if (!gui || !gui->view || !gui->realized)
        return;

    PuglStatus st = puglStartTimer(gui->view, RENDER_TIMER_ID, 1.0 / 60.0);


}

void m4a_gui_stop_internal_timer(M4AGuiState *gui)
{
    if (!gui || !gui->view)
        return;

    puglStopTimer(gui->view, RENDER_TIMER_ID);

}

void m4a_gui_set_project_assets(M4AGuiState *gui,
                                const ProjectAssetEntry *directsoundAssets,
                                int directsoundCount,
                                const ProjectAssetEntry *progWaveAssets,
                                int progWaveCount,
                                const ProjectAssetOverride *overrides)
{
    if (!gui) return;
    gui->directsoundAssets = directsoundAssets;
    gui->directsoundAssetCount = directsoundCount;
    gui->progWaveAssets = progWaveAssets;
    gui->progWaveAssetCount = progWaveCount;
    gui->assetOverrides = overrides;
}

void m4a_gui_set_plugin_data(M4AGuiState *gui, void *plugin_data)
{
    if (!gui) return;
    gui->plugin_data = plugin_data;
}

void m4a_gui_set_audio_api(M4AGuiState *gui, const M4AHostAudioApi *api)
{
    if (!gui) return;
    if (api) {
        gui->audioApi = *api;
        gui->audioDevicesDirty = true;
    } else {
        memset(&gui->audioApi, 0, sizeof(gui->audioApi));
        gui->audioDeviceCount = 0;
    }
}

bool m4a_gui_poll_sample_swap(M4AGuiState *gui, int *voiceIndex,
                              ProjectAssetKind *kind,
                              char *fileName, int fileNameSize)
{
    if (!gui || !gui->pendingSampleSwap)
        return false;
    *voiceIndex = gui->pendingSwapVoice;
    *kind = gui->pendingSwapKind;
    strncpy(fileName, gui->pendingSwapFileName, (size_t)fileNameSize - 1);
    fileName[fileNameSize - 1] = '\0';
    gui->pendingSampleSwap = false;
    return true;
}
} /* extern "C" */
