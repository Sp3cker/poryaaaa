#include "midi_timeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} MidiReader;

typedef struct {
    uint64_t tick;
    uint32_t tempo;
} TempoEvent;

typedef struct {
    uint64_t tick;
    uint8_t channel;
    uint8_t track;
    uint8_t type;
    uint8_t data0;
    uint8_t data1;
    int origIndex;
} RawMidiEvent;

typedef struct {
    RawMidiEvent *events;
    int count;
    int capacity;
} RawEventArray;

typedef struct {
    TempoEvent *events;
    int count;
    int capacity;
} TempoArray;

static int mr_read_byte(MidiReader *r, uint8_t *out)
{
    if (r->pos >= r->size) return -1;
    *out = r->data[r->pos++];
    return 0;
}

static int mr_read_u16_be(MidiReader *r, uint16_t *out)
{
    if (r->pos + 2 > r->size) return -1;
    *out = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return 0;
}

static int mr_read_u32_be(MidiReader *r, uint32_t *out)
{
    if (r->pos + 4 > r->size) return -1;
    *out = ((uint32_t)r->data[r->pos] << 24)
         | ((uint32_t)r->data[r->pos + 1] << 16)
         | ((uint32_t)r->data[r->pos + 2] << 8)
         | (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    return 0;
}

static int mr_skip(MidiReader *r, uint32_t n)
{
    if (r->pos + n > r->size) return -1;
    r->pos += n;
    return 0;
}

static int mr_read_vlq(MidiReader *r, uint32_t *out)
{
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (mr_read_byte(r, &b) < 0) return -1;
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) {
            *out = val;
            return 0;
        }
    }
    return -1;
}

static int raw_push(RawEventArray *a, RawMidiEvent ev)
{
    if (a->count >= a->capacity) {
        int nc = a->capacity ? a->capacity * 2 : 512;
        RawMidiEvent *p = realloc(a->events, (size_t)nc * sizeof(*p));
        if (!p) return -1;
        a->events = p;
        a->capacity = nc;
    }
    ev.origIndex = a->count;
    a->events[a->count++] = ev;
    return 0;
}

static int tempo_push(TempoArray *a, TempoEvent ev)
{
    if (a->count >= a->capacity) {
        int nc = a->capacity ? a->capacity * 2 : 16;
        TempoEvent *p = realloc(a->events, (size_t)nc * sizeof(*p));
        if (!p) return -1;
        a->events = p;
        a->capacity = nc;
    }
    a->events[a->count++] = ev;
    return 0;
}

static bool text_is_loop_marker(const uint8_t *buf, uint32_t len, char marker)
{
    uint32_t s = 0;
    uint32_t e = len;
    while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r' || buf[s] == '\n'))
        s++;
    while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\r' || buf[e - 1] == '\n'))
        e--;
    return (e - s == 1) && ((char)buf[s] == marker);
}

static void parse_track(MidiReader *r, uint32_t trackLen, uint8_t trackIndex,
                        RawEventArray *rawEvents, TempoArray *tempos,
                        uint64_t *loopStartTick, uint64_t *loopEndTick)
{
    size_t end = r->pos + trackLen;
    uint64_t tick = 0;
    uint8_t runningStatus = 0;

    while (r->pos < end) {
        uint32_t delta;
        if (mr_read_vlq(r, &delta) < 0) break;
        tick += delta;

        uint8_t b;
        if (mr_read_byte(r, &b) < 0) break;

        if (b == 0xFF) {
            runningStatus = 0;
            uint8_t metaType;
            uint32_t metaLen;
            if (mr_read_byte(r, &metaType) < 0) break;
            if (mr_read_vlq(r, &metaLen) < 0) break;

            if (metaType == 0x51 && metaLen == 3) {
                uint8_t t0, t1, t2;
                if (mr_read_byte(r, &t0) < 0 ||
                    mr_read_byte(r, &t1) < 0 ||
                    mr_read_byte(r, &t2) < 0) break;
                TempoEvent te = {
                    tick,
                    ((uint32_t)t0 << 16) | ((uint32_t)t1 << 8) | t2
                };
                tempo_push(tempos, te);
            } else if (metaType >= 0x01 && metaType <= 0x07) {
                uint32_t readLen = metaLen < 32 ? metaLen : 32;
                uint8_t textBuf[32];
                for (uint32_t j = 0; j < readLen; j++) {
                    if (mr_read_byte(r, &textBuf[j]) < 0) goto track_done;
                }
                if (metaLen > readLen && mr_skip(r, metaLen - readLen) < 0)
                    goto track_done;
                if (text_is_loop_marker(textBuf, readLen, '[') && *loopStartTick == UINT64_MAX)
                    *loopStartTick = tick;
                else if (text_is_loop_marker(textBuf, readLen, ']') && *loopEndTick == UINT64_MAX)
                    *loopEndTick = tick;
            } else {
                if (mr_skip(r, metaLen) < 0) break;
            }
        } else if (b == 0xF0 || b == 0xF7) {
            runningStatus = 0;
            uint32_t sysexLen;
            if (mr_read_vlq(r, &sysexLen) < 0) break;
            if (mr_skip(r, sysexLen) < 0) break;
        } else {
            uint8_t status;
            uint8_t data0;
            if (b & 0x80) {
                status = b;
                runningStatus = b;
                if (mr_read_byte(r, &data0) < 0) break;
            } else {
                if (!runningStatus) break;
                status = runningStatus;
                data0 = b;
            }

            uint8_t type = (status >> 4) & 0x0F;
            uint8_t chan = status & 0x0F;

            switch (type) {
            case 0x8: {
                uint8_t vel;
                if (mr_read_byte(r, &vel) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, 0x8, data0, vel, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0x9: {
                uint8_t vel;
                if (mr_read_byte(r, &vel) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, vel ? (uint8_t)0x9 : (uint8_t)0x8, data0, vel, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0xA: {
                uint8_t dummy;
                if (mr_read_byte(r, &dummy) < 0) goto track_done;
                break;
            }
            case 0xB: {
                uint8_t val;
                if (mr_read_byte(r, &val) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, 0xB, data0, val, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0xC: {
                RawMidiEvent ev = { tick, chan, trackIndex, 0xC, data0, 0, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0xD:
                break;
            case 0xE: {
                uint8_t msb;
                if (mr_read_byte(r, &msb) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, 0xE, data0, msb, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            default:
                goto track_done;
            }
        }
    }

track_done:
    r->pos = end;
}

static int cmp_raw_events(const void *a, const void *b)
{
    const RawMidiEvent *ea = (const RawMidiEvent *)a;
    const RawMidiEvent *eb = (const RawMidiEvent *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return 1;
    if (ea->origIndex < eb->origIndex) return -1;
    if (ea->origIndex > eb->origIndex) return 1;
    return 0;
}

static int cmp_tempo_events(const void *a, const void *b)
{
    uint64_t ta = ((const TempoEvent *)a)->tick;
    uint64_t tb = ((const TempoEvent *)b)->tick;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return 0;
}

static uint64_t tick_to_sample(uint64_t tick,
                               const TempoEvent *tempos, int tempoCount,
                               uint32_t tpqn, double sampleRate)
{
    double samples = 0.0;
    uint64_t prevTick = 0;
    double prevTempo = 500000.0;

    for (int i = 0; i < tempoCount; i++) {
        if (tempos[i].tick >= tick) break;
        uint64_t segTicks = tempos[i].tick - prevTick;
        samples += (double)segTicks * prevTempo / (double)tpqn / 1000000.0 * sampleRate;
        prevTick = tempos[i].tick;
        prevTempo = (double)tempos[i].tempo;
    }

    samples += (double)(tick - prevTick) * prevTempo / (double)tpqn / 1000000.0 * sampleRate;
    return (uint64_t)(samples + 0.5);
}

MidiTimeline *midi_timeline_load(const char *path, double sampleRate)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open MIDI file: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0) {
        fprintf(stderr, "Empty MIDI file\n");
        fclose(f);
        return NULL;
    }

    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "Failed to read MIDI file: %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    MidiReader r = { buf, (size_t)fsize, 0 };
    if (r.size < 14 || memcmp(r.data, "MThd", 4) != 0) {
        fprintf(stderr, "Not a Standard MIDI File: %s\n", path);
        free(buf);
        return NULL;
    }

    r.pos = 4;

    uint32_t hdrLen;
    uint16_t format;
    uint16_t numTracks;
    uint16_t division;
    if (mr_read_u32_be(&r, &hdrLen) < 0 ||
        mr_read_u16_be(&r, &format) < 0 ||
        mr_read_u16_be(&r, &numTracks) < 0 ||
        mr_read_u16_be(&r, &division) < 0) {
        fprintf(stderr, "Invalid MIDI header\n");
        free(buf);
        return NULL;
    }
    if (hdrLen > 6) r.pos += hdrLen - 6;

    if (format > 1) {
        fprintf(stderr, "Unsupported MIDI format %u (only 0 and 1 supported)\n", format);
        free(buf);
        return NULL;
    }
    if (division & 0x8000) {
        fprintf(stderr, "SMPTE time codes not supported\n");
        free(buf);
        return NULL;
    }

    RawEventArray rawEvents = { NULL, 0, 0 };
    TempoArray tempos = { NULL, 0, 0 };
    uint64_t loopStartTick = UINT64_MAX;
    uint64_t loopEndTick = UINT64_MAX;
    uint32_t tpqn = division;

    for (int t = 0; t < (int)numTracks; t++) {
        if (r.pos + 8 > r.size) break;
        if (memcmp(r.data + r.pos, "MTrk", 4) != 0) {
            fprintf(stderr, "Expected MTrk chunk (track %d)\n", t);
            break;
        }
        r.pos += 4;
        uint32_t trackLen;
        if (mr_read_u32_be(&r, &trackLen) < 0) break;
        size_t trackEnd = r.pos + trackLen;
        parse_track(&r, trackLen, (uint8_t)t, &rawEvents, &tempos, &loopStartTick, &loopEndTick);
        r.pos = trackEnd;
    }

    free(buf);

    if (rawEvents.count > 0)
        qsort(rawEvents.events, (size_t)rawEvents.count, sizeof(RawMidiEvent), cmp_raw_events);
    if (tempos.count > 0)
        qsort(tempos.events, (size_t)tempos.count, sizeof(TempoEvent), cmp_tempo_events);

    MidiTimeline *timeline = calloc(1, sizeof(*timeline));
    if (!timeline) {
        free(rawEvents.events);
        free(tempos.events);
        return NULL;
    }

    timeline->events = malloc((size_t)rawEvents.count * sizeof(*timeline->events));
    if (rawEvents.count > 0 && !timeline->events) {
        midi_timeline_free(timeline);
        free(rawEvents.events);
        free(tempos.events);
        return NULL;
    }

    timeline->count = rawEvents.count;
    timeline->midiFormat = format;
    timeline->numTracks = numTracks;
    timeline->loopStartSample = UINT64_MAX;
    timeline->loopEndSample = UINT64_MAX;

    uint64_t lastTick = 0;
    for (int i = 0; i < rawEvents.count; i++) {
        const RawMidiEvent *re = &rawEvents.events[i];
        if (re->tick > lastTick)
            lastTick = re->tick;
        timeline->events[i].samplePos = tick_to_sample(re->tick, tempos.events, tempos.count, tpqn, sampleRate);
        timeline->events[i].channel = re->channel;
        timeline->events[i].track = re->track;
        timeline->events[i].type = re->type;
        timeline->events[i].data0 = re->data0;
        timeline->events[i].data1 = re->data1;
    }
    timeline->totalMidiSamples = tick_to_sample(lastTick, tempos.events, tempos.count, tpqn, sampleRate);

    if (loopStartTick != UINT64_MAX) {
        timeline->loopStartSample = tick_to_sample(loopStartTick, tempos.events, tempos.count, tpqn, sampleRate);
    }
    if (loopEndTick != UINT64_MAX) {
        timeline->loopEndSample = tick_to_sample(loopEndTick, tempos.events, tempos.count, tpqn, sampleRate);
    }

    for (int i = 0; i < timeline->count; i++) {
        int trackIndex = midi_timeline_track_index(timeline, &timeline->events[i]);
        if (trackIndex >= 0 && trackIndex < MAX_TRACKS)
            timeline->trackUsed[trackIndex] = true;
    }

    free(rawEvents.events);
    free(tempos.events);
    return timeline;
}

void midi_timeline_free(MidiTimeline *timeline)
{
    if (!timeline) return;
    free(timeline->events);
    free(timeline);
}

int midi_timeline_track_index(const MidiTimeline *timeline, const MidiRenderEvent *event)
{
    if (!timeline || !event) return -1;
    return timeline->midiFormat == 1 ? (int)event->track : (int)event->channel;
}
