#ifndef M4A_PARAMS_H
#define M4A_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

#include <clap/clap.h>
#include <clap/ext/params.h>

typedef struct M4APluginData M4APluginData;

void m4a_params_init(M4APluginData *data);
void m4a_params_apply_to_engine(M4APluginData *data);

bool m4a_params_state_save(const M4APluginData *data, const clap_ostream_t *stream);
bool m4a_params_state_load(M4APluginData *data, const clap_istream_t *stream);

bool m4a_params_process_value_event(M4APluginData *data,
                                    const clap_event_param_value_t *event);
bool m4a_params_process_midi_program_change(M4APluginData *data,
                                            int channel,
                                            uint8_t program);

const clap_plugin_params_t *m4a_params_extension(void);

#endif /* M4A_PARAMS_H */
