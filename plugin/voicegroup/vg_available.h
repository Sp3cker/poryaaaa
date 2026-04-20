#ifndef VG_AVAILABLE_H
#define VG_AVAILABLE_H

#include "voicegroup_loader.h"
#include "vg_paths.h"

/*
 * An "available instrument" — one entry the user can append to a
 * voicegroup file. Two kinds: DirectSound samples (voice_directsound)
 * and keysplits (voice_keysplit / voice_keysplit_all).
 *
 * The display `name` is the user-facing label (ccomidi dropdown).
 * The `macro` is the ready-to-append line: poryaaaa writes it
 * verbatim into the voicegroup .inc when the user asks to add this.
 */
typedef struct {
    char name[128];
    char macro[384];
} AvailableInstrument;

typedef struct {
    AvailableInstrument *entries;
    int count;
    int capacity;
} AvailableInstrumentList;

void vg_available_init(AvailableInstrumentList *list);
void vg_available_free(AvailableInstrumentList *list);

/*
 * Walk the project and build the list.
 *
 * DirectSound samples are enumerated from direct_sound_data.inc files.
 * A sample is omitted from the list when every reference to it comes
 * from a keysplit sub-voicegroup — the keysplit itself is listed instead.
 *
 * Keysplits are discovered by scanning voicegroup files for
 * voice_keysplit / voice_keysplit_all macros and collecting unique
 * (sub_voicegroup, routing_table) pairs.
 */
void vg_available_build(const char *projectRoot,
                        const VoicegroupLoaderConfig *config,
                        AvailableInstrumentList *out);

#endif /* VG_AVAILABLE_H */
