#include "m4a_params.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m4a_engine.h"
#include "m4a_plugin.h"

enum {
    M4A_PROGRAM_PARAM_BASE = 0x4D345000u,
};

typedef struct {
    uint8_t effectivePrograms[MAX_TRACKS];
    uint8_t automatedPrograms[MAX_TRACKS];
    uint8_t midiProgramOverride[MAX_TRACKS];
} M4AParamsStateSnapshot;

static bool param_id_to_channel(clap_id param_id, int *channel_out)
{
    if (param_id < M4A_PROGRAM_PARAM_BASE ||
        param_id >= M4A_PROGRAM_PARAM_BASE + MAX_TRACKS)
        return false;

    *channel_out = (int)(param_id - M4A_PROGRAM_PARAM_BASE);
    return true;
}

static clap_id channel_to_param_id(int channel)
{
    return (clap_id)(M4A_PROGRAM_PARAM_BASE + channel);
}

static uint8_t clamp_program_value(double value)
{
    if (value < 0.0)
        return 0;
    if (value > 127.0)
        return 127;
    return (uint8_t)value;
}

static uint8_t load_program_value(const atomic_uchar *value)
{
    return atomic_load_explicit(value, memory_order_acquire);
}

static void store_program_value(atomic_uchar *dst, uint8_t value)
{
    atomic_store_explicit(dst, value, memory_order_release);
}

static bool load_override_flag(const atomic_bool *value)
{
    return atomic_load_explicit(value, memory_order_acquire);
}

static void store_override_flag(atomic_bool *dst, bool value)
{
    atomic_store_explicit(dst, value, memory_order_release);
}

static void capture_state_snapshot(const M4APluginData *data, M4AParamsStateSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    for (int i = 0; i < MAX_TRACKS; ++i) {
        snapshot->effectivePrograms[i] = load_program_value(&data->effectivePrograms[i]);
        snapshot->automatedPrograms[i] = load_program_value(&data->automatedPrograms[i]);
        snapshot->midiProgramOverride[i] = load_override_flag(&data->midiProgramOverride[i]) ? 1 : 0;
    }
}

static void restore_state_snapshot(M4APluginData *data, const M4AParamsStateSnapshot *snapshot)
{
    for (int i = 0; i < MAX_TRACKS; ++i) {
        store_program_value(&data->effectivePrograms[i], snapshot->effectivePrograms[i] & 0x7F);
        store_program_value(&data->automatedPrograms[i], snapshot->automatedPrograms[i] & 0x7F);
        store_override_flag(&data->midiProgramOverride[i],
                            snapshot->midiProgramOverride[i] != 0);
    }
}

static void apply_effective_program(M4APluginData *data, int channel, uint8_t program)
{
    if (channel < 0 || channel >= MAX_TRACKS)
        return;

    store_program_value(&data->effectivePrograms[channel], program);
    if (data->activated)
        m4a_engine_program_change(&data->engine, channel, program);
}

static const char *skip_spaces(const char *text)
{
    while (*text && isspace((unsigned char)*text))
        ++text;
    return text;
}

static bool consume_word(const char **text, const char *word)
{
    const char *cursor = *text;
    while (*word && *cursor &&
           tolower((unsigned char)*cursor) == tolower((unsigned char)*word)) {
        ++cursor;
        ++word;
    }

    if (*word != '\0')
        return false;

    *text = cursor;
    return true;
}

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return MAX_TRACKS;
}

static bool params_get_info(const clap_plugin_t *plugin,
                            uint32_t param_index,
                            clap_param_info_t *param_info)
{
    (void)plugin;

    if (param_index >= MAX_TRACKS || !param_info)
        return false;

    memset(param_info, 0, sizeof(*param_info));
    param_info->id = channel_to_param_id((int)param_index);
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE |
                        CLAP_PARAM_IS_STEPPED |
                        CLAP_PARAM_IS_ENUM |
                        CLAP_PARAM_REQUIRES_PROCESS;
    snprintf(param_info->name, sizeof(param_info->name),
             "Voice Channel %u Program Change", param_index + 1);
    snprintf(param_info->module, sizeof(param_info->module),
             "Voice Channel %u", param_index + 1);
    param_info->min_value = 0.0;
    param_info->max_value = 127.0;
    param_info->default_value = 0.0;
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id param_id, double *out_value)
{
    const M4APluginData *data = (const M4APluginData *)plugin->plugin_data;
    int channel = 0;

    if (!out_value || !param_id_to_channel(param_id, &channel))
        return false;

    *out_value = (double)load_program_value(&data->effectivePrograms[channel]);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin,
                                 clap_id param_id,
                                 double value,
                                 char *out_buffer,
                                 uint32_t out_buffer_capacity)
{
    int channel = 0;
    uint8_t program = 0;

    (void)plugin;

    if (!out_buffer || out_buffer_capacity == 0 || !param_id_to_channel(param_id, &channel))
        return false;

    program = clamp_program_value(value);
    snprintf(out_buffer, out_buffer_capacity, "Program %u", (unsigned)program);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin,
                                 clap_id param_id,
                                 const char *param_value_text,
                                 double *out_value)
{
    const char *cursor = param_value_text;
    char *end = NULL;
    long parsed = 0;
    bool has_program_prefix = false;
    int channel = 0;

    (void)plugin;

    if (!out_value || !param_value_text || !param_id_to_channel(param_id, &channel))
        return false;

    cursor = skip_spaces(cursor);
    if (consume_word(&cursor, "program")) {
        has_program_prefix = true;
        cursor = skip_spaces(cursor);
    }

    parsed = strtol(cursor, &end, 10);
    if (cursor == end)
        return false;

    end = (char *)skip_spaces(end);
    if (*end != '\0')
        return false;

    if (has_program_prefix && (parsed < 0 || parsed > 127))
        return false;
    if (!has_program_prefix && (parsed < 0 || parsed > 127))
        return false;

    *out_value = (double)parsed;

    return true;
}

static void params_flush(const clap_plugin_t *plugin,
                         const clap_input_events_t *in,
                         const clap_output_events_t *out)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    uint32_t num_events = 0;

    (void)out;

    if (!in)
        return;

    num_events = in->size(in);
    for (uint32_t i = 0; i < num_events; ++i) {
        const clap_event_header_t *header = in->get(in, i);
        if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;

        switch (header->type) {
        case CLAP_EVENT_PARAM_VALUE:
            m4a_params_process_value_event(data, (const clap_event_param_value_t *)header);
            break;
        case CLAP_EVENT_MIDI:
        {
            const clap_event_midi_t *midi_event = (const clap_event_midi_t *)header;
            if ((midi_event->data[0] & 0xF0) == 0xC0)
                m4a_params_process_midi_program_change(
                    data,
                    midi_event->data[0] & 0x0F,
                    midi_event->data[1] & 0x7F);
            break;
        }
        }
    }
}

static const clap_plugin_params_t s_params = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush = params_flush,
};

void m4a_params_init(M4APluginData *data)
{
    for (int i = 0; i < MAX_TRACKS; ++i) {
        atomic_init(&data->effectivePrograms[i], 0);
        atomic_init(&data->automatedPrograms[i], 0);
        atomic_init(&data->midiProgramOverride[i], false);
    }
}

void m4a_params_apply_to_engine(M4APluginData *data)
{
    for (int i = 0; i < MAX_TRACKS; ++i)
        m4a_engine_program_change(&data->engine, i, load_program_value(&data->effectivePrograms[i]));
}

bool m4a_params_state_save(const M4APluginData *data, const clap_ostream_t *stream)
{
    M4AParamsStateSnapshot snapshot;

    if (!stream)
        return false;

    capture_state_snapshot(data, &snapshot);
    return stream->write(stream, &snapshot, sizeof(snapshot)) == (int64_t)sizeof(snapshot);
}

bool m4a_params_state_load(M4APluginData *data, const clap_istream_t *stream)
{
    M4AParamsStateSnapshot snapshot;
    int64_t bytes_read = 0;

    if (!stream)
        return false;

    memset(&snapshot, 0, sizeof(snapshot));
    bytes_read = stream->read(stream, &snapshot, sizeof(snapshot));
    if (bytes_read < 0)
        return false;
    if (bytes_read > 0 && bytes_read != (int64_t)sizeof(snapshot))
        return false;

    restore_state_snapshot(data, &snapshot);
    return true;
}

bool m4a_params_process_value_event(M4APluginData *data,
                                    const clap_event_param_value_t *event)
{
    uint8_t program = 0;
    uint8_t previous_automation = 0;
    bool midi_override = false;
    int channel = 0;

    if (!event || !param_id_to_channel(event->param_id, &channel))
        return false;

    program = clamp_program_value(event->value);
    previous_automation = load_program_value(&data->automatedPrograms[channel]);
    midi_override = load_override_flag(&data->midiProgramOverride[channel]);

    store_program_value(&data->automatedPrograms[channel], program);

    /* A MIDI PC latches over repeated automation of the previous target.
     * Automation regains control once it actually changes to a new program. */
    if (midi_override && program == previous_automation)
        return true;

    store_override_flag(&data->midiProgramOverride[channel], false);
    apply_effective_program(data, channel, program);
    return true;
}

bool m4a_params_process_midi_program_change(M4APluginData *data,
                                            int channel,
                                            uint8_t program)
{
    if (!data || channel < 0 || channel >= MAX_TRACKS)
        return false;

    store_override_flag(&data->midiProgramOverride[channel], true);
    apply_effective_program(data, channel, program & 0x7F);

    return true;
}

const clap_plugin_params_t *m4a_params_extension(void)
{
    return &s_params;
}
