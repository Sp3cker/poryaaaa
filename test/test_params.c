#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#include "m4a_params.h"
#include "m4a_plugin.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b, msg) do { \
    tests_run++; \
    if ((int)(a) != (int)(b)) { \
        fprintf(stderr, "FAIL: %s: expected %d, got %d (line %d)\n", \
                msg, (int)(b), (int)(a), __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

typedef struct {
    clap_ostream_t stream;
    uint8_t buffer[256];
    size_t size;
} MemoryOStream;

typedef struct {
    clap_istream_t stream;
    const uint8_t *buffer;
    size_t size;
    size_t pos;
} MemoryIStream;

static int64_t memory_stream_write(const clap_ostream_t *stream,
                                   const void *buffer,
                                   uint64_t size)
{
    MemoryOStream *self = (MemoryOStream *)stream->ctx;
    if (self->size + size > sizeof(self->buffer))
        return -1;

    memcpy(self->buffer + self->size, buffer, (size_t)size);
    self->size += (size_t)size;
    return (int64_t)size;
}

static int64_t memory_stream_read(const clap_istream_t *stream,
                                  void *buffer,
                                  uint64_t size)
{
    MemoryIStream *self = (MemoryIStream *)stream->ctx;
    size_t remaining = self->size - self->pos;
    size_t to_copy = (size_t)size;

    if (remaining == 0)
        return 0;
    if (to_copy > remaining)
        to_copy = remaining;

    memcpy(buffer, self->buffer + self->pos, to_copy);
    self->pos += to_copy;
    return (int64_t)to_copy;
}

static void init_plugin(clap_plugin_t *plugin, M4APluginData *data)
{
    memset(plugin, 0, sizeof(*plugin));
    memset(data, 0, sizeof(*data));
    plugin->plugin_data = data;
    m4a_params_init(data);
}

static clap_event_param_value_t make_param_event(clap_id param_id, double value)
{
    clap_event_param_value_t event;
    memset(&event, 0, sizeof(event));
    event.header.size = sizeof(event);
    event.header.type = CLAP_EVENT_PARAM_VALUE;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.param_id = param_id;
    event.note_id = -1;
    event.port_index = -1;
    event.channel = -1;
    event.key = -1;
    event.value = value;
    return event;
}

static void test_params_extension_basics(void)
{
    clap_plugin_t plugin;
    M4APluginData data;
    clap_param_info_t info;
    const clap_plugin_params_t *params = m4a_params_extension();
    double value = -1.0;
    char text[64];

    printf("Testing params extension basics...\n");

    init_plugin(&plugin, &data);

    ASSERT_EQ_INT(params->count(&plugin), MAX_TRACKS, "16 program params exposed");
    ASSERT(params->get_info(&plugin, 0, &info), "param 0 info available");
    ASSERT(strcmp(info.name, "Voice Channel 1 Program Change") == 0,
           "param name identifies voice channel");
    ASSERT(strcmp(info.module, "Voice Channel 1") == 0, "param module identifies voice channel");
    ASSERT_EQ_INT((int)info.min_value, 0, "param min is 0");
    ASSERT_EQ_INT((int)info.max_value, 127, "param max is 127");
    ASSERT(params->get_value(&plugin, info.id, &value), "param get_value works");
    ASSERT_EQ_INT((int)value, 0, "default program is 0");
    ASSERT(params->value_to_text(&plugin, info.id, 0.0, text, sizeof(text)),
           "value_to_text works");
    ASSERT(strcmp(text, "Program 0") == 0, "display text is 0-based");
    ASSERT(params->text_to_value(&plugin, info.id, "Program 0", &value),
           "text_to_value parses prefixed 0-based text");
    ASSERT_EQ_INT((int)value, 0, "prefixed 0-based text parses to program 0");
}

static void test_midi_override_behavior(void)
{
    clap_plugin_t plugin;
    M4APluginData data;
    clap_param_info_t info;
    clap_event_param_value_t event;
    const clap_plugin_params_t *params = m4a_params_extension();
    double value = -1.0;

    printf("Testing MIDI override behavior...\n");

    init_plugin(&plugin, &data);
    ASSERT(params->get_info(&plugin, 0, &info), "param info available for override test");

    event = make_param_event(info.id, 7.0);
    ASSERT(m4a_params_process_value_event(&data, &event), "automation value applied");
    ASSERT(params->get_value(&plugin, info.id, &value), "value readable after automation");
    ASSERT_EQ_INT((int)value, 7, "automation sets effective program");

    ASSERT(m4a_params_process_midi_program_change(&data, 0, 11),
           "midi program change applied");
    ASSERT(params->get_value(&plugin, info.id, &value), "value readable after MIDI PC");
    ASSERT_EQ_INT((int)value, 11, "MIDI program change overrides effective program");

    ASSERT(m4a_params_process_value_event(&data, &event), "old automation value accepted");
    ASSERT(params->get_value(&plugin, info.id, &value), "value readable after repeated automation");
    ASSERT_EQ_INT((int)value, 11, "repeated old automation stays overridden by MIDI");

    event.value = 9.0;
    ASSERT(m4a_params_process_value_event(&data, &event), "new automation value applied");
    ASSERT(params->get_value(&plugin, info.id, &value), "value readable after new automation");
    ASSERT_EQ_INT((int)value, 9, "new automation retakes control");
}

static void test_state_roundtrip_preserves_override(void)
{
    clap_plugin_t plugin;
    clap_plugin_t restored_plugin;
    M4APluginData data;
    M4APluginData restored_data;
    clap_param_info_t info;
    clap_event_param_value_t event;
    MemoryOStream out;
    MemoryIStream in;
    const clap_plugin_params_t *params = m4a_params_extension();
    double value = -1.0;

    printf("Testing params state roundtrip...\n");

    init_plugin(&plugin, &data);
    ASSERT(params->get_info(&plugin, 0, &info), "param info available for state test");

    event = make_param_event(info.id, 4.0);
    ASSERT(m4a_params_process_value_event(&data, &event), "automation baseline applied");
    ASSERT(m4a_params_process_midi_program_change(&data, 0, 6),
           "midi override established");

    memset(&out, 0, sizeof(out));
    out.stream.ctx = &out;
    out.stream.write = memory_stream_write;
    ASSERT(m4a_params_state_save(&data, &out.stream), "params state saved");

    init_plugin(&restored_plugin, &restored_data);
    memset(&in, 0, sizeof(in));
    in.stream.ctx = &in;
    in.stream.read = memory_stream_read;
    in.buffer = out.buffer;
    in.size = out.size;
    ASSERT(m4a_params_state_load(&restored_data, &in.stream), "params state loaded");

    ASSERT(params->get_value(&restored_plugin, info.id, &value), "restored value readable");
    ASSERT_EQ_INT((int)value, 6, "effective MIDI-overridden program restored");

    event.value = 4.0;
    ASSERT(m4a_params_process_value_event(&restored_data, &event),
           "restored old automation value accepted");
    ASSERT(params->get_value(&restored_plugin, info.id, &value),
           "value readable after restored old automation");
    ASSERT_EQ_INT((int)value, 6, "restored old automation is still overridden");

    event.value = 8.0;
    ASSERT(m4a_params_process_value_event(&restored_data, &event),
           "restored new automation value accepted");
    ASSERT(params->get_value(&restored_plugin, info.id, &value),
           "value readable after restored new automation");
    ASSERT_EQ_INT((int)value, 8, "restored new automation regains control");
}

int main(void)
{
    printf("=== M4A Params Tests ===\n\n");

    test_params_extension_basics();
    test_midi_override_behavior();
    test_state_roundtrip_preserves_override();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
