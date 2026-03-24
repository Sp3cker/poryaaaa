#ifndef MIDI_TIMELINE_H
#define MIDI_TIMELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "m4a_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t samplePos;
    uint8_t channel;
    uint8_t track;
    uint8_t type;
    uint8_t data0;
    uint8_t data1;
} MidiRenderEvent;

typedef struct {
    MidiRenderEvent *events;
    int count;
    uint64_t totalMidiSamples;
    uint64_t loopStartSample;
    uint64_t loopEndSample;
    uint16_t midiFormat;
    uint16_t numTracks;
    bool trackUsed[MAX_TRACKS];
} MidiTimeline;

MidiTimeline *midi_timeline_load(const char *path, double sampleRate);
void midi_timeline_free(MidiTimeline *timeline);
int midi_timeline_track_index(const MidiTimeline *timeline, const MidiRenderEvent *event);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_TIMELINE_H */
