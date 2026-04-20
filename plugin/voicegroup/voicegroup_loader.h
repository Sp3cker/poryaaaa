#ifndef VOICEGROUP_LOADER_H
#define VOICEGROUP_LOADER_H

#include "voicegroup_types.h"

#define VOICEGROUP_SIZE 128
#define VG_MAX_PATH_LEN 512
#define VG_MAX_VOICE_SAMPLE_NAME 128

/*
 * Optional configuration for the voicegroup loader.
 * All paths are relative to the project root directory.
 * Zero-initialized config means "auto-discover everything".
 */
typedef struct {
    char soundDataPaths[8][VG_MAX_PATH_LEN];    /* extra .inc files with sample symbol definitions */
    int soundDataPathCount;
    char voicegroupPaths[8][VG_MAX_PATH_LEN];   /* extra voicegroup directories or files */
    int voicegroupPathCount;
} VoicegroupLoaderConfig;

/*
 * Loaded voicegroup data - holds all allocated resources.
 * Must be freed with voicegroup_free() when done.
 */
typedef struct {
    ToneData voices[VOICEGROUP_SIZE];

    /* Per-slot sample basename (e.g. "brass_1.bin", "wave_01.pcm").
     * Empty string for slots that reference no sample (square, noise,
     * keysplit, keysplit_all) or for unused slots.
     * Populated by the parser for DirectSound, ProgrammableWave, and
     * Cry voices. Intended for consumers that want to display voices
     * to a user without reloading the project's symbol maps. */
    char voiceSampleNames[VOICEGROUP_SIZE][VG_MAX_VOICE_SAMPLE_NAME];

    /* Loaded wave data (samples) */
    WaveData **waveDatas;
    int waveDataCount;
    int waveDataCapacity;

    /* Loaded programmable wave data */
    uint32_t **progWaves;
    int progWaveCount;
    int progWaveCapacity;

    /* Sub-voicegroups (keysplits, drumsets) */
    ToneData **subGroups;
    int subGroupCount;
    int subGroupCapacity;

    /* Keysplit tables */
    uint8_t **keySplitTables;
    int keySplitTableCount;
    int keySplitTableCapacity;

    /* Absolute path of the .inc/.s file that defines this voicegroup.
     * For emerald-style per-file layouts this is the file to append to
     * when adding an instrument. Empty for monolithic layouts. */
    char sourceFile[VG_MAX_PATH_LEN];
} LoadedVoiceGroup;

/*
 * Load a voicegroup from a project.
 *
 * projectRoot: path to the project root directory
 * voicegroupName: name of the voicegroup (e.g. "petalburg", "voicegroup000")
 * config: optional loader configuration (NULL for pure auto-discovery)
 *
 * The loader auto-discovers project structure (pokeemerald, pokefirered,
 * and forks with custom sound directories). Config overrides can
 * specify additional search paths.
 *
 * Returns a LoadedVoiceGroup on success, or NULL on failure.
 * The caller must free the result with voicegroup_free().
 */
LoadedVoiceGroup *voicegroup_load(const char *projectRoot, const char *voicegroupName,
                                   const VoicegroupLoaderConfig *config);

/*
 * Free all resources associated with a loaded voicegroup.
 */
void voicegroup_free(LoadedVoiceGroup *vg);

/*
 * Set an optional file path for diagnostic logging inside the voicegroup loader.
 * Pass NULL to disable. The same path used by the plugin's "log=" config key works.
 * Call before voicegroup_load() for the output to be useful.
 */
void voicegroup_loader_set_log_path(const char *path);

/* ---- Project asset collection (for voicegroup sample swapper) ---- */

typedef enum {
    PROJECT_ASSET_DIRECTSOUND,
    PROJECT_ASSET_PROG_WAVE,
} ProjectAssetKind;

typedef struct {
    ProjectAssetKind kind;
    char fileName[256];               /* basename visible to user, e.g. "brass_1.wav" */
    char relPath[VG_MAX_PATH_LEN];    /* path relative to project root */
    char symbol[256];                 /* assembly symbol name */
} ProjectAssetEntry;

typedef struct {
    ProjectAssetEntry *directsound;
    int directsoundCount;
    ProjectAssetEntry *progWave;
    int progWaveCount;
} VoicegroupProjectAssets;

/*
 * Collect all DirectSound and ProgrammableWave sample assets from a project.
 * Populates out->directsound and out->progWave with malloc'd arrays.
 * Returns true on success. Caller must free with voicegroup_loader_free_project_assets().
 */
bool voicegroup_loader_collect_project_assets(const char *projectRoot,
                                              const VoicegroupLoaderConfig *config,
                                              VoicegroupProjectAssets *out);

void voicegroup_loader_free_project_assets(VoicegroupProjectAssets *assets);

/*
 * Load a DirectSound sample by its relative path and register it with the voicegroup.
 * Returns the loaded WaveData, or NULL on failure.
 */
WaveData *voicegroup_loader_load_sample(const char *projectRoot,
                                        const char *relPath,
                                        LoadedVoiceGroup *vg);

/*
 * Load a programmable wave by its relative path and register it with the voicegroup.
 * Returns the loaded data, or NULL on failure.
 */
uint32_t *voicegroup_loader_load_prog_wave(const char *projectRoot,
                                           const char *relPath,
                                           LoadedVoiceGroup *vg);

#endif /* VOICEGROUP_LOADER_H */
