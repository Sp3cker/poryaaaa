#include "m4a_standalone_player.h"

#include <stdio.h>
#include <string.h>

static void reset_transport(M4AStandalonePlayer *player,
                            M4AEngine *engine,
                            bool startPlaying,
                            bool hasVoiceGroup)
{
    bool muted[MAX_TRACKS];

    memcpy(muted, player->trackMuted, sizeof(muted));
    m4a_engine_all_sound_off(engine);
    m4a_reverb_reset(&engine->reverb);
    engine->lowPassLeft = 0.0f;
    engine->lowPassRight = 0.0f;
    memcpy(player->trackMuted, muted, sizeof(player->trackMuted));
    memset(player->trackPrograms, 0, sizeof(player->trackPrograms));
    player->currentSample = 0;
    player->nextEventIndex = 0;
    player->positionSeconds = 0.0;
    player->isPlaying = startPlaying && player->timeline && hasVoiceGroup;
}

static void dispatch_event(M4AStandalonePlayer *player,
                           M4AEngine *engine,
                           const MidiRenderEvent *ev)
{
    int trackIdx;

    if (!player || !engine || !ev)
        return;

    trackIdx = midi_timeline_track_index(player->timeline, ev);
    if (trackIdx < 0 || trackIdx >= MAX_TRACKS)
        return;

    player->trackChannels[trackIdx] = ev->channel;

    if (player->trackMuted[trackIdx] && ev->type == 0x9)
        return;

    switch (ev->type) {
    case 0x8:
        m4a_engine_note_off(engine, trackIdx, ev->data0);
        break;
    case 0x9:
        m4a_engine_note_on(engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xB:
        m4a_engine_cc(engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xC:
        player->trackPrograms[trackIdx] = ev->data0;
        m4a_engine_program_change(engine, trackIdx, ev->data0);
        break;
    case 0xE:
    {
        int16_t bend = (int16_t)(((int)(ev->data1 << 7) | ev->data0) - 8192);
        m4a_engine_pitch_bend(engine, trackIdx, bend);
        break;
    }
    }
}

static bool load_midi(M4AStandalonePlayer *player,
                      M4AEngine *engine,
                      ToneData *voiceGroup,
                      double sampleRate,
                      const char *path)
{
    MidiTimeline *timeline;

    if (!path || !path[0])
        return false;

    timeline = midi_timeline_load(path, sampleRate);
    if (!timeline)
        return false;

    midi_timeline_free(player->timeline);
    player->timeline = timeline;
    snprintf(player->midiPath, sizeof(player->midiPath), "%s", path);
    memset(player->trackUsed, 0, sizeof(player->trackUsed));
    memset(player->trackChannels, 0, sizeof(player->trackChannels));
    memset(player->trackPrograms, 0, sizeof(player->trackPrograms));

    for (int i = 0; i < timeline->count; i++) {
        int trackIdx = midi_timeline_track_index(timeline, &timeline->events[i]);
        if (trackIdx < 0 || trackIdx >= MAX_TRACKS)
            continue;
        player->trackUsed[trackIdx] = true;
        player->trackChannels[trackIdx] = timeline->events[i].channel;
    }

    player->totalPlaybackSamples = timeline->totalMidiSamples
                                 + (uint64_t)(player->tailSeconds * sampleRate + 0.5);
    player->midiLoaded = true;
    player->totalSeconds = sampleRate > 0.0
                         ? (double)player->totalPlaybackSamples / sampleRate
                         : 0.0;

    reset_transport(player, engine, false, voiceGroup != NULL);
    if (voiceGroup)
        m4a_engine_set_voicegroup(engine, voiceGroup);
    return true;
}

static void seek_transport(M4AStandalonePlayer *player,
                           M4AEngine *engine,
                           ToneData *voiceGroup,
                           double sampleRate,
                           double seconds)
{
    bool wasPlaying;
    uint64_t targetSample;

    if (!player->timeline || !voiceGroup)
        return;

    if (seconds < 0.0)
        seconds = 0.0;
    if (player->totalSeconds > 0.0 && seconds > player->totalSeconds)
        seconds = player->totalSeconds;

    wasPlaying = player->isPlaying;
    targetSample = (uint64_t)(seconds * sampleRate + 0.5);

    reset_transport(player, engine, false, voiceGroup != NULL);
    m4a_engine_set_voicegroup(engine, voiceGroup);
    player->currentSample = targetSample > player->totalPlaybackSamples
                          ? player->totalPlaybackSamples
                          : targetSample;

    while (player->nextEventIndex < player->timeline->count &&
           player->timeline->events[player->nextEventIndex].samplePos <= player->currentSample) {
        dispatch_event(player, engine, &player->timeline->events[player->nextEventIndex]);
        player->nextEventIndex++;
    }

    player->positionSeconds = sampleRate > 0.0
                            ? (double)player->currentSample / sampleRate
                            : 0.0;
    player->isPlaying = wasPlaying && player->currentSample < player->totalPlaybackSamples;
}

static void apply_track_mutes(M4AStandalonePlayer *player, M4AEngine *engine)
{
    for (int i = 0; i < MAX_TRACKS; i++) {
        bool wasMuted = player->trackMuted[i];
        bool muted = player->pendingTrackMuted[i];
        player->trackMuted[i] = muted;
        if (!wasMuted && muted)
            m4a_engine_all_notes_off(engine, i);
    }
}

void m4a_standalone_player_init(M4AStandalonePlayer *player, bool enabled)
{
    if (!player)
        return;

    memset(player, 0, sizeof(*player));
    player->enabled = enabled;
    player->tailSeconds = 3.0;
}

void m4a_standalone_player_destroy(M4AStandalonePlayer *player)
{
    if (!player)
        return;

    midi_timeline_free(player->timeline);
    player->timeline = NULL;
}

void m4a_standalone_player_set_config(M4AStandalonePlayer *player,
                                      const char *midiPath,
                                      double tailSeconds)
{
    if (!player)
        return;

    if (midiPath && midiPath[0])
        snprintf(player->midiPath, sizeof(player->midiPath), "%s", midiPath);
    if (tailSeconds >= 0.0)
        player->tailSeconds = tailSeconds;
}

void m4a_standalone_player_on_activate(M4AStandalonePlayer *player,
                                       M4AEngine *engine,
                                       ToneData *voiceGroup,
                                       double sampleRate)
{
    if (!player || !player->enabled)
        return;

    if (player->timeline) {
        char path[sizeof(player->midiPath)];
        snprintf(path, sizeof(path), "%s", player->midiPath);
        midi_timeline_free(player->timeline);
        player->timeline = NULL;
        player->midiLoaded = false;
        player->totalPlaybackSamples = 0;
        player->totalSeconds = 0.0;
        if (path[0])
            load_midi(player, engine, voiceGroup, sampleRate, path);
    } else if (player->midiPath[0]) {
        load_midi(player, engine, voiceGroup, sampleRate, player->midiPath);
    }

    if (!player->timeline) {
        player->isPlaying = false;
        player->positionSeconds = 0.0;
        player->totalSeconds = 0.0;
        player->totalPlaybackSamples = 0;
    }
}

void m4a_standalone_player_fill_gui_state(const M4AStandalonePlayer *player,
                                          M4AGuiPlayerState *state)
{
    if (!state)
        return;

    memset(state, 0, sizeof(*state));
    if (!player)
        return;

    state->enabled = player->enabled;
    state->midiLoaded = player->midiLoaded;
    state->isPlaying = player->isPlaying;
    state->positionSeconds = player->positionSeconds;
    state->totalSeconds = player->totalSeconds;
    memcpy(state->midiPath, player->midiPath, sizeof(state->midiPath));
    memcpy(state->trackUsed, player->trackUsed, sizeof(state->trackUsed));
    memcpy(state->trackMuted, player->trackMuted, sizeof(state->trackMuted));
    memcpy(state->trackChannels, player->trackChannels, sizeof(state->trackChannels));
    memcpy(state->trackPrograms, player->trackPrograms, sizeof(state->trackPrograms));
}

void m4a_standalone_player_enqueue_actions(M4AStandalonePlayer *player,
                                           const M4AGuiPlayerActions *actions)
{
    if (!player || !player->enabled || !actions)
        return;

    if (actions->loadMidi) {
        snprintf(player->pendingMidiPath, sizeof(player->pendingMidiPath),
                 "%s", actions->midiPath);
        atomic_store_explicit(&player->loadMidiRequested, true, memory_order_release);
    }
    if (actions->togglePlayPause)
        atomic_store_explicit(&player->togglePlayPauseRequested, true, memory_order_release);
    if (actions->stop)
        atomic_store_explicit(&player->stopRequested, true, memory_order_release);
    if (actions->restart)
        atomic_store_explicit(&player->restartRequested, true, memory_order_release);
    if (actions->seek) {
        player->pendingSeekSeconds = actions->seekSeconds;
        atomic_store_explicit(&player->seekRequested, true, memory_order_release);
    }
    if (actions->trackMuteChanged) {
        memcpy(player->pendingTrackMuted, actions->trackMuted, sizeof(player->pendingTrackMuted));
        atomic_store_explicit(&player->trackMuteChangeRequested, true, memory_order_release);
    }
}

void m4a_standalone_player_consume_actions(M4AStandalonePlayer *player,
                                           M4AEngine *engine,
                                           ToneData *voiceGroup,
                                           double sampleRate)
{
    if (!player || !player->enabled || !engine)
        return;

    if (atomic_exchange_explicit(&player->loadMidiRequested, false, memory_order_acq_rel))
        load_midi(player, engine, voiceGroup, sampleRate, player->pendingMidiPath);

    if (atomic_exchange_explicit(&player->trackMuteChangeRequested, false, memory_order_acq_rel))
        apply_track_mutes(player, engine);

    if (atomic_exchange_explicit(&player->seekRequested, false, memory_order_acq_rel))
        seek_transport(player, engine, voiceGroup, sampleRate, player->pendingSeekSeconds);

    if (atomic_exchange_explicit(&player->stopRequested, false, memory_order_acq_rel))
        reset_transport(player, engine, false, voiceGroup != NULL);

    if (atomic_exchange_explicit(&player->restartRequested, false, memory_order_acq_rel))
        reset_transport(player, engine, true, voiceGroup != NULL);

    if (atomic_exchange_explicit(&player->togglePlayPauseRequested, false, memory_order_acq_rel)) {
        if (player->timeline && voiceGroup) {
            if (!player->isPlaying && player->currentSample >= player->totalPlaybackSamples)
                reset_transport(player, engine, true, voiceGroup != NULL);
            else
                player->isPlaying = !player->isPlaying;
        }
    }
}

void m4a_standalone_player_dispatch_due_events(M4AStandalonePlayer *player,
                                               M4AEngine *engine)
{
    if (!player || !player->enabled || !player->isPlaying || !player->timeline)
        return;

    while (player->nextEventIndex < player->timeline->count &&
           player->timeline->events[player->nextEventIndex].samplePos <= player->currentSample) {
        dispatch_event(player, engine, &player->timeline->events[player->nextEventIndex]);
        player->nextEventIndex++;
    }
}

uint32_t m4a_standalone_player_next_event_offset(const M4AStandalonePlayer *player,
                                                 uint32_t maxFrames)
{
    uint64_t delta = maxFrames;
    uint64_t stopDelta;

    if (!player || !player->enabled || !player->isPlaying || !player->timeline)
        return maxFrames;

    if (player->currentSample < player->totalPlaybackSamples) {
        stopDelta = player->totalPlaybackSamples - player->currentSample;
        if (stopDelta < delta)
            delta = stopDelta;
    }

    if (player->nextEventIndex < player->timeline->count) {
        if (player->timeline->events[player->nextEventIndex].samplePos <= player->currentSample)
            return 0;

        stopDelta = player->timeline->events[player->nextEventIndex].samplePos - player->currentSample;
        if (stopDelta < delta)
            delta = stopDelta;
    }

    if (delta > maxFrames)
        return maxFrames;
    return (uint32_t)delta;
}

void m4a_standalone_player_advance(M4AStandalonePlayer *player, uint32_t frames)
{
    if (!player || !player->enabled)
        return;

    if (player->isPlaying) {
        player->currentSample += frames;
        if (player->currentSample >= player->totalPlaybackSamples) {
            player->currentSample = player->totalPlaybackSamples;
            player->isPlaying = false;
        }
    }

    if (player->totalPlaybackSamples > 0)
        player->positionSeconds = player->totalSeconds
                                * (double)player->currentSample
                                / (double)player->totalPlaybackSamples;
    else
        player->positionSeconds = 0.0;
}
