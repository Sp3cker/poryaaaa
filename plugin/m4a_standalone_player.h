#ifndef M4A_STANDALONE_PLAYER_H
#define M4A_STANDALONE_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "m4a_engine.h"
#include "m4a_gui.h"
#include "midi_timeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    char midiPath[512];
    double tailSeconds;

    MidiTimeline *timeline;
    bool midiLoaded;
    bool isPlaying;
    double positionSeconds;
    double totalSeconds;
    bool trackUsed[MAX_TRACKS];
    bool trackMuted[MAX_TRACKS];
    uint8_t trackChannels[MAX_TRACKS];
    uint8_t trackPrograms[MAX_TRACKS];
    uint64_t totalPlaybackSamples;
    uint64_t currentSample;
    int nextEventIndex;

    char pendingMidiPath[512];
    bool pendingTrackMuted[MAX_TRACKS];
    double pendingSeekSeconds;
    atomic_bool loadMidiRequested;
    atomic_bool togglePlayPauseRequested;
    atomic_bool stopRequested;
    atomic_bool restartRequested;
    atomic_bool seekRequested;
    atomic_bool trackMuteChangeRequested;
} M4AStandalonePlayer;

void m4a_standalone_player_init(M4AStandalonePlayer *player, bool enabled);
void m4a_standalone_player_destroy(M4AStandalonePlayer *player);
void m4a_standalone_player_set_config(M4AStandalonePlayer *player,
                                      const char *midiPath,
                                      double tailSeconds);
void m4a_standalone_player_on_activate(M4AStandalonePlayer *player,
                                       M4AEngine *engine,
                                       ToneData *voiceGroup,
                                       double sampleRate);
void m4a_standalone_player_fill_gui_state(const M4AStandalonePlayer *player,
                                          M4AGuiPlayerState *state);
void m4a_standalone_player_enqueue_actions(M4AStandalonePlayer *player,
                                           const M4AGuiPlayerActions *actions);
void m4a_standalone_player_consume_actions(M4AStandalonePlayer *player,
                                           M4AEngine *engine,
                                           ToneData *voiceGroup,
                                           double sampleRate);
void m4a_standalone_player_dispatch_due_events(M4AStandalonePlayer *player,
                                               M4AEngine *engine);
uint32_t m4a_standalone_player_next_event_offset(const M4AStandalonePlayer *player,
                                                 uint32_t maxFrames);
void m4a_standalone_player_advance(M4AStandalonePlayer *player, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* M4A_STANDALONE_PLAYER_H */
