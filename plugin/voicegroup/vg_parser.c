#include "vg_parser.h"
#include "vg_log.h"
#include "vg_paths.h"
#include "vg_wav.h"

#include "voicegroup_types.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 64

/*
 * Where a voicegroup lives on disk. `filePath` is the .inc/.s path.
 * When `label` is non-empty the voicegroup lives inside a monolithic
 * file at `<label>::` rather than being its own file.
 */
typedef struct {
    char filePath[VG_MAX_PATH_LEN];
    char label[VG_MAX_SYMBOL_LEN];
    int found;
} VoicegroupLocation;

/* ---- Dynamic-array registration helpers ---- */
/* Each register_* call appends to an owned list on the
 * LoadedVoiceGroup; voicegroup_free() frees those lists. */

static void register_wavedata(LoadedVoiceGroup *vg, WaveData *wd)
{
    if (vg->waveDataCount >= vg->waveDataCapacity) {
        vg->waveDataCapacity = vg->waveDataCapacity ? vg->waveDataCapacity * 2 : INITIAL_CAPACITY;
        vg->waveDatas = realloc(vg->waveDatas, sizeof(WaveData *) * vg->waveDataCapacity);
    }
    vg->waveDatas[vg->waveDataCount++] = wd;
}

static void register_progwave(LoadedVoiceGroup *vg, uint32_t *pw)
{
    if (vg->progWaveCount >= vg->progWaveCapacity) {
        vg->progWaveCapacity = vg->progWaveCapacity ? vg->progWaveCapacity * 2 : INITIAL_CAPACITY;
        vg->progWaves = realloc(vg->progWaves, sizeof(uint32_t *) * vg->progWaveCapacity);
    }
    vg->progWaves[vg->progWaveCount++] = pw;
}

static void register_subgroup(LoadedVoiceGroup *vg, ToneData *sg)
{
    if (vg->subGroupCount >= vg->subGroupCapacity) {
        vg->subGroupCapacity = vg->subGroupCapacity ? vg->subGroupCapacity * 2 : INITIAL_CAPACITY;
        vg->subGroups = realloc(vg->subGroups, sizeof(ToneData *) * vg->subGroupCapacity);
    }
    vg->subGroups[vg->subGroupCount++] = sg;
}

static void register_keysplittable(LoadedVoiceGroup *vg, uint8_t *ks)
{
    if (vg->keySplitTableCount >= vg->keySplitTableCapacity) {
        vg->keySplitTableCapacity = vg->keySplitTableCapacity ? vg->keySplitTableCapacity * 2 : INITIAL_CAPACITY;
        vg->keySplitTables = realloc(vg->keySplitTables, sizeof(uint8_t *) * vg->keySplitTableCapacity);
    }
    vg->keySplitTables[vg->keySplitTableCount++] = ks;
}

/* ---- Parse context ---- */
/* Every voice-macro handler needs access to the same small bundle of
 * inputs. Threading them through 15+ call sites as individual args
 * was error-prone; packaging into one struct makes handler
 * signatures uniform and short. */

typedef struct {
    const char *projectRoot;
    const SymbolMap *dsMap;
    const SymbolMap *pwMap;
    const KeySplitMap *ksMap;
    const ProjectDiscovery *disc;
    LoadedVoiceGroup *vg;
    WaveCache *waveCache;
    /* Trailing "@ ..." comment from the current voice macro's line, trimmed
     * of surrounding whitespace. NULL/empty if the line had no comment.
     * Valid only for the duration of the current dispatch; handlers that
     * recurse (keysplit -> load_sub_voicegroup) must capture it first. */
    const char *lineComment;
} ParseCtx;

/* ---- Sample resolution ---- */

/*
 * Resolve a DirectSound sample symbol to a WaveData via the symbol
 * map (looking up the .bin path pokeemerald records there, then
 * substituting .wav on disk). Returns NULL if the symbol isn't in
 * direct_sound_data.inc — matching real-ROM linking, where only
 * .incbin'd samples are part of the build. Cached within the
 * ParseCtx so the same .wav isn't loaded twice per voicegroup.
 */
static WaveData *resolve_sample(const ParseCtx *ctx, const char *symbol)
{
    const char *samplePath = vg_symbol_map_find(ctx->dsMap, symbol);
    if (samplePath) {
        /* Cache key is the absolute .wav path (swap .bin extension). */
        char relWavPath[VG_MAX_PATH_LEN];
        strncpy(relWavPath, samplePath, VG_MAX_PATH_LEN - 1);
        relWavPath[VG_MAX_PATH_LEN - 1] = '\0';
        size_t pathLen = strlen(relWavPath);
        if (pathLen >= 4 && strcmp(relWavPath + pathLen - 4, ".bin") == 0) {
            relWavPath[pathLen - 3] = 'w';
            relWavPath[pathLen - 2] = 'a';
            relWavPath[pathLen - 1] = 'v';
        }
        char absWavPath[VG_MAX_PATH_LEN];
        vg_build_path(absWavPath, sizeof(absWavPath), ctx->projectRoot, relWavPath);

        WaveData *cached = vg_wave_cache_find(ctx->waveCache, absWavPath);
        if (cached) return cached;

        WaveData *wd = vg_load_sample(ctx->projectRoot, samplePath);
        if (wd) {
            register_wavedata(ctx->vg, wd);
            vg_wave_cache_insert(ctx->waveCache, absWavPath, wd);
            return wd;
        }
    }

    return NULL;
}

/*
 * Load a programmable-wave symbol and register it with the voicegroup.
 * Returns NULL if not found.
 */
static uint32_t *resolve_prog_wave(const ParseCtx *ctx, const char *symbol)
{
    const char *wavePath = vg_symbol_map_find(ctx->pwMap, symbol);
    if (!wavePath) return NULL;
    uint32_t *pw = vg_load_prog_wave(ctx->projectRoot, wavePath);
    if (pw) register_progwave(ctx->vg, pw);
    return pw;
}

/*
 * Record a user-visible sample name on the voice slot. Called by any
 * handler that resolves a sample/wave/cry symbol so GUIs can show
 * the list of instruments without reloading the symbol map.
 *
 * If the symbol resolves in `map`, we store the basename of the
 * resolved path (e.g. "brass_1.bin"). If not (e.g. the wav-dirs
 * fallback path took over), we fall back to the raw symbol name.
 */
static void set_slot_name(ParseCtx *ctx, ToneData *td, const char *name)
{
    int slot = (int)(td - ctx->vg->voices);
    if (slot < 0 || slot >= VOICEGROUP_SIZE) return;
    strncpy(ctx->vg->voiceSampleNames[slot], name, VG_MAX_VOICE_SAMPLE_NAME - 1);
    ctx->vg->voiceSampleNames[slot][VG_MAX_VOICE_SAMPLE_NAME - 1] = '\0';
}

static void record_sample_name(ParseCtx *ctx, ToneData *td,
                               const SymbolMap *map, const char *symbol)
{
    const char *path = vg_symbol_map_find(map, symbol);
    set_slot_name(ctx, td, path ? vg_path_basename(path) : symbol);
}

/*
 * Cry voices deliberately bypass the wav cache and sample dir fallback
 * — pokeemerald's cry samples are always referenced via the symbol map
 * and loaded once per voicegroup.
 */
static WaveData *resolve_cry_sample(const ParseCtx *ctx, const char *symbol)
{
    const char *samplePath = vg_symbol_map_find(ctx->dsMap, symbol);
    if (!samplePath) return NULL;
    WaveData *wd = vg_load_bin_sample(ctx->projectRoot, samplePath);
    if (wd) register_wavedata(ctx->vg, wd);
    return wd;
}

/* ---- Voicegroup file location ---- */

/* True iff the last path component of dirPath equals name. */
static int dir_last_component_is(const char *dirPath, const char *name)
{
    size_t dlen = strlen(dirPath);
    size_t nlen = strlen(name);
    if (nlen > dlen) return 0;
    const char *tail = dirPath + dlen - nlen;
    if (strcmp(tail, name) != 0) return 0;
    if (tail == dirPath) return 1;
    char prev = *(tail - 1);
    return prev == '/' || prev == '\\';
}

static int set_if_exists(const char *path, VoicegroupLocation *out)
{
    if (!vg_file_exists(path)) return 0;
    strncpy(out->filePath, path, VG_MAX_PATH_LEN - 1);
    out->found = 1;
    return 1;
}

/* Try <dir>/<name>.inc then <dir>/<name>.s. Returns 1 if found. */
static int try_file_in_dir(const char *dir, const char *name, VoicegroupLocation *out)
{
    char path[VG_MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%c%s.inc", dir, VG_PATH_SEP, name);
    if (set_if_exists(path, out)) return 1;
    snprintf(path, sizeof(path), "%s%c%s.s", dir, VG_PATH_SEP, name);
    return set_if_exists(path, out);
}

/* Try <dir>/<subdir>/<name>.inc then .s. */
static int try_file_in_subdir(const char *dir, const char *subdir, const char *name,
                              VoicegroupLocation *out)
{
    char path[VG_MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s%c%s%c%s.inc", dir, VG_PATH_SEP, subdir, VG_PATH_SEP, name);
    if (set_if_exists(path, out)) return 1;
    snprintf(path, sizeof(path), "%s%c%s%c%s.s", dir, VG_PATH_SEP, subdir, VG_PATH_SEP, name);
    return set_if_exists(path, out);
}

/*
 * Handle the <base>_keysplit / <base>_drumset naming convention.
 * When a voice_keysplit macro references e.g. "petalburg_keysplit",
 * the sub-voicegroup typically lives at
 * sound/voicegroups/keysplits/petalburg.inc. Only dirs whose last
 * path component matches (or dirs that have a "keysplits"/"drumsets"
 * subdir) are searched — searching every voicegroup dir would match
 * petalburg.inc itself and recurse forever.
 */
static int try_suffix_convention(const char *vgName, const char *suffix,
                                 const char *subdir,
                                 const ProjectDiscovery *disc,
                                 VoicegroupLocation *out)
{
    const char *found = strstr(vgName, suffix);
    if (!found) return 0;

    char baseName[VG_MAX_SYMBOL_LEN];
    int baseLen = (int)(found - vgName);
    if (baseLen <= 0 || baseLen >= VG_MAX_SYMBOL_LEN) return 0;
    memcpy(baseName, vgName, baseLen);
    baseName[baseLen] = '\0';

    /* <voicegroupDir>/<subdir>/<base>.inc */
    for (int i = 0; i < disc->voicegroupDirs.count; i++)
        if (try_file_in_subdir(disc->voicegroupDirs.paths[i], subdir, baseName, out))
            return 1;

    /* Directories that ARE the subdir (e.g. ".../keysplits"). */
    for (int i = 0; i < disc->voicegroupDirs.count; i++) {
        if (!dir_last_component_is(disc->voicegroupDirs.paths[i], subdir)) continue;
        if (try_file_in_dir(disc->voicegroupDirs.paths[i], baseName, out)) return 1;
    }
    return 0;
}

/*
 * Scan a monolithic file for "<vgName>::" and record its location.
 */
static int find_in_monolithic_files(const char *vgName,
                                    const ProjectDiscovery *disc,
                                    VoicegroupLocation *out)
{
    char searchLabel[VG_MAX_SYMBOL_LEN + 4];
    snprintf(searchLabel, sizeof(searchLabel), "%s::", vgName);

    for (int i = 0; i < disc->monolithicVGFiles.count; i++) {
        FILE *f = fopen(disc->monolithicVGFiles.paths[i], "r");
        if (!f) continue;
        char line[VG_MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            vg_strip_comment(line);
            char *trimmed = vg_ltrim(line);
            if (strstr(trimmed, searchLabel) == trimmed) {
                strncpy(out->filePath, disc->monolithicVGFiles.paths[i], VG_MAX_PATH_LEN - 1);
                strncpy(out->label, vgName, VG_MAX_SYMBOL_LEN - 1);
                out->found = 1;
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

static VoicegroupLocation find_voicegroup(const char *vgName, const ProjectDiscovery *disc)
{
    VoicegroupLocation loc;
    memset(&loc, 0, sizeof(loc));

    /* 1. <dir>/<name>.{inc,s} in every discovered voicegroup dir. */
    for (int i = 0; i < disc->voicegroupDirs.count; i++)
        if (try_file_in_dir(disc->voicegroupDirs.paths[i], vgName, &loc))
            return loc;

    /* 2. <base>_keysplit / <base>_drumset suffix conventions. */
    if (try_suffix_convention(vgName, "_keysplit", "keysplits", disc, &loc)) return loc;
    if (try_suffix_convention(vgName, "_drumset", "drumsets", disc, &loc)) return loc;

    /* 3. vg_<name>.{inc,s} (eventide convention). */
    char vgPrefixed[VG_MAX_SYMBOL_LEN];
    snprintf(vgPrefixed, sizeof(vgPrefixed), "vg_%s", vgName);
    for (int i = 0; i < disc->voicegroupDirs.count; i++)
        if (try_file_in_dir(disc->voicegroupDirs.paths[i], vgPrefixed, &loc))
            return loc;

    /* 4. Labels inside monolithic voice_groups.inc. */
    find_in_monolithic_files(vgName, disc, &loc);
    return loc;
}

/* ---- Voice macro dispatch ---- */

typedef enum {
    MK_DIRECTSOUND,     /* key, pan, sample_sym, A, D, S, R */
    MK_SQUARE_1,        /* key, pan, sweep, duty, A, D, S, R */
    MK_SQUARE_2,        /* key, pan, duty, A, D, S, R */
    MK_PROG_WAVE,       /* key, pan, wave_sym, A, D, S, R */
    MK_NOISE,           /* key, pan, period, A, D, S, R */
    MK_KEYSPLIT,        /* vg_sym, ks_sym */
    MK_KEYSPLIT_ALL,    /* vg_sym */
    MK_CRY,             /* sample_sym */
} MacroKind;

typedef struct {
    const char *keyword;
    uint8_t voiceType;
    MacroKind kind;
} VoiceMacro;

/*
 * Match order matters: more specific variants (_no_resample, _alt)
 * MUST come before their shorter base forms so the prefix match
 * doesn't fire early.
 */
static const VoiceMacro kVoiceMacros[] = {
    { "voice_directsound_no_resample", VOICE_DIRECTSOUND_NO_RESAMPLE, MK_DIRECTSOUND },
    { "voice_directsound_alt",         VOICE_DIRECTSOUND_ALT,         MK_DIRECTSOUND },
    { "voice_directsound",             VOICE_DIRECTSOUND,             MK_DIRECTSOUND },
    { "voice_square_1_alt",            VOICE_SQUARE_1_ALT,            MK_SQUARE_1 },
    { "voice_square_1",                VOICE_SQUARE_1,                MK_SQUARE_1 },
    { "voice_square_2_alt",            VOICE_SQUARE_2_ALT,            MK_SQUARE_2 },
    { "voice_square_2",                VOICE_SQUARE_2,                MK_SQUARE_2 },
    { "voice_programmable_wave_alt",   VOICE_PROGRAMMABLE_WAVE_ALT,   MK_PROG_WAVE },
    { "voice_programmable_wave",       VOICE_PROGRAMMABLE_WAVE,       MK_PROG_WAVE },
    { "voice_noise_alt",               VOICE_NOISE_ALT,               MK_NOISE },
    { "voice_noise",                   VOICE_NOISE,                   MK_NOISE },
    { "voice_keysplit_all",            VOICE_KEYSPLIT_ALL,            MK_KEYSPLIT_ALL },
    { "voice_keysplit",                VOICE_KEYSPLIT,                MK_KEYSPLIT },
    { "cry_reverse",                   VOICE_CRY_REVERSE,             MK_CRY },
    { "cry",                           VOICE_CRY,                     MK_CRY },
    { NULL, 0, 0 },
};

/* Forward declaration: parser and sub-voicegroup loader are mutually
 * recursive (a voice_keysplit references another voicegroup). */
static int parse_voicegroup_file(const char *filePath, const char *startLabel,
                                 ParseCtx *ctx);

static ToneData *load_sub_voicegroup(const char *vgSymbol, ParseCtx *ctx)
{
    const char *name = vgSymbol;
    if (strncmp(name, "voicegroup_", 11) == 0)
        name += 11;

    VoicegroupLocation loc = find_voicegroup(name, ctx->disc);
    if (!loc.found) {
        vg_err("cannot find sub-voicegroup '%s'", vgSymbol);
        return NULL;
    }

    ToneData *subVg = calloc(VOICEGROUP_SIZE, sizeof(ToneData));
    if (!subVg) return NULL;

    /* parse_voicegroup_file writes into vg->voices and, via
     * record_sample_name, into vg->voiceSampleNames. Save/restore both
     * so the recursion doesn't clobber the parent voicegroup's data. */
    ToneData savedVoices[VOICEGROUP_SIZE];
    char savedNames[VOICEGROUP_SIZE][VG_MAX_VOICE_SAMPLE_NAME];
    memcpy(savedVoices, ctx->vg->voices, sizeof(savedVoices));
    memcpy(savedNames,  ctx->vg->voiceSampleNames, sizeof(savedNames));
    memset(ctx->vg->voices, 0, sizeof(ctx->vg->voices));
    memset(ctx->vg->voiceSampleNames, 0, sizeof(ctx->vg->voiceSampleNames));

    const char *startLabel = loc.label[0] ? loc.label : NULL;
    int rc = parse_voicegroup_file(loc.filePath, startLabel, ctx);
    if (rc != 0) {
        free(subVg);
        memcpy(ctx->vg->voices, savedVoices, sizeof(savedVoices));
        memcpy(ctx->vg->voiceSampleNames, savedNames, sizeof(savedNames));
        return NULL;
    }

    memcpy(subVg, ctx->vg->voices, sizeof(ToneData) * VOICEGROUP_SIZE);
    memcpy(ctx->vg->voices, savedVoices, sizeof(savedVoices));
    memcpy(ctx->vg->voiceSampleNames, savedNames, sizeof(savedNames));

    register_subgroup(ctx->vg, subVg);
    return subVg;
}

/* ---- Per-macro-kind handlers ---- */

static void handle_directsound(ToneData *td, uint8_t voiceType,
                               const char *args, ParseCtx *ctx)
{
    int key, pan, attack, decay, sustain, release;
    char sampleSymbol[VG_MAX_SYMBOL_LEN];
    if (sscanf(args, "%d, %d, %[^,], %d, %d, %d, %d",
               &key, &pan, sampleSymbol, &attack, &decay, &sustain, &release) != 7)
        return;

    vg_rtrim(sampleSymbol);
    td->type     = voiceType;
    td->key      = (uint8_t)key;
    td->panSweep = pan ? (uint8_t)(0x80 | pan) : 0;
    td->attack   = (uint8_t)attack;
    td->decay    = (uint8_t)decay;
    td->sustain  = (uint8_t)sustain;
    td->release  = (uint8_t)release;

    WaveData *wd = resolve_sample(ctx, sampleSymbol);
    if (wd) {
        td->wav = wd;
        record_sample_name(ctx, td, ctx->dsMap, sampleSymbol);
    }
}

static void handle_square_1(ToneData *td, uint8_t voiceType,
                            const char *args, ParseCtx *ctx)
{
    (void)ctx;
    int key, pan, sweep, duty, attack, decay, sustain, release;
    if (sscanf(args, "%d, %d, %d, %d, %d, %d, %d, %d",
               &key, &pan, &sweep, &duty, &attack, &decay, &sustain, &release) != 8)
        return;

    td->type        = voiceType;
    td->key         = (uint8_t)key;
    td->panSweep    = (uint8_t)sweep;
    td->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
    td->attack      = (uint8_t)(attack & 0x07);
    td->decay       = (uint8_t)(decay  & 0x07);
    td->sustain     = (uint8_t)(sustain & 0x0F);
    td->release     = (uint8_t)(release & 0x07);
}

static void handle_square_2(ToneData *td, uint8_t voiceType,
                            const char *args, ParseCtx *ctx)
{
    (void)ctx;
    int key, pan, duty, attack, decay, sustain, release;
    if (sscanf(args, "%d, %d, %d, %d, %d, %d, %d",
               &key, &pan, &duty, &attack, &decay, &sustain, &release) != 7)
        return;

    td->type        = voiceType;
    td->key         = (uint8_t)key;
    td->panSweep    = 0;
    td->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
    td->attack      = (uint8_t)(attack & 0x07);
    td->decay       = (uint8_t)(decay  & 0x07);
    td->sustain     = (uint8_t)(sustain & 0x0F);
    td->release     = (uint8_t)(release & 0x07);
}

static void handle_prog_wave(ToneData *td, uint8_t voiceType,
                             const char *args, ParseCtx *ctx)
{
    int key, pan, attack, decay, sustain, release;
    char waveSymbol[VG_MAX_SYMBOL_LEN];
    if (sscanf(args, "%d, %d, %[^,], %d, %d, %d, %d",
               &key, &pan, waveSymbol, &attack, &decay, &sustain, &release) != 7)
        return;

    vg_rtrim(waveSymbol);
    td->type    = voiceType;
    td->key     = (uint8_t)key;
    td->attack  = (uint8_t)(attack & 0x07);
    td->decay   = (uint8_t)(decay  & 0x07);
    td->sustain = (uint8_t)(sustain & 0x0F);
    td->release = (uint8_t)(release & 0x07);

    uint32_t *pw = resolve_prog_wave(ctx, waveSymbol);
    if (pw) {
        td->wavePointer = pw;
        record_sample_name(ctx, td, ctx->pwMap, waveSymbol);
    }
}

static void handle_noise(ToneData *td, uint8_t voiceType,
                         const char *args, ParseCtx *ctx)
{
    (void)ctx;
    int key, pan, period, attack, decay, sustain, release;
    if (sscanf(args, "%d, %d, %d, %d, %d, %d, %d",
               &key, &pan, &period, &attack, &decay, &sustain, &release) != 7)
        return;

    td->type        = voiceType;
    td->key         = (uint8_t)key;
    td->wavePointer = (uint32_t *)(uintptr_t)(period & 0x01);
    td->attack      = (uint8_t)(attack & 0x07);
    td->decay       = (uint8_t)(decay  & 0x07);
    td->sustain     = (uint8_t)(sustain & 0x0F);
    td->release     = (uint8_t)(release & 0x07);
}

static void handle_keysplit(ToneData *td, uint8_t voiceType,
                            const char *args, ParseCtx *ctx)
{
    char vgSymbol[VG_MAX_SYMBOL_LEN];
    char ksSymbol[VG_MAX_SYMBOL_LEN];
    if (sscanf(args, "%[^,], %s", vgSymbol, ksSymbol) != 2)
        return;

    /* Capture before recursing — load_sub_voicegroup overwrites lineComment. */
    char displayName[VG_MAX_VOICE_SAMPLE_NAME];
    if (ctx->lineComment && ctx->lineComment[0])
        snprintf(displayName, sizeof(displayName), "%s", ctx->lineComment);
    else
        snprintf(displayName, sizeof(displayName), "%s", vgSymbol);

    vg_rtrim(vgSymbol);
    vg_rtrim(ksSymbol);
    td->type     = voiceType;
    td->subGroup = load_sub_voicegroup(vgSymbol, ctx);
    set_slot_name(ctx, td, displayName);

    KeySplitDef *ksDef = vg_keysplit_map_find(ctx->ksMap, ksSymbol);
    if (ksDef) {
        uint8_t *table = malloc(128);
        memcpy(table, ksDef->table, 128);
        td->keySplitTable = table;
        register_keysplittable(ctx->vg, table);
    }
}

static void handle_keysplit_all(ToneData *td, uint8_t voiceType,
                                const char *args, ParseCtx *ctx)
{
    char vgSymbol[VG_MAX_SYMBOL_LEN];
    if (sscanf(args, "%s", vgSymbol) != 1) return;

    char displayName[VG_MAX_VOICE_SAMPLE_NAME];
    if (ctx->lineComment && ctx->lineComment[0])
        snprintf(displayName, sizeof(displayName), "%s", ctx->lineComment);
    else
        snprintf(displayName, sizeof(displayName), "%s", vgSymbol);

    vg_rtrim(vgSymbol);
    td->type     = voiceType;
    td->subGroup = load_sub_voicegroup(vgSymbol, ctx);
    set_slot_name(ctx, td, displayName);
}

static void handle_cry(ToneData *td, uint8_t voiceType,
                       const char *args, ParseCtx *ctx)
{
    char sampleSymbol[VG_MAX_SYMBOL_LEN];
    if (sscanf(args, "%s", sampleSymbol) != 1) return;
    vg_rtrim(sampleSymbol);

    td->type    = voiceType;
    td->key     = 60;
    td->attack  = 0xFF;
    td->decay   = 0;
    td->sustain = 0xFF;
    td->release = 0;

    WaveData *wd = resolve_cry_sample(ctx, sampleSymbol);
    if (wd) {
        td->wav = wd;
        record_sample_name(ctx, td, ctx->dsMap, sampleSymbol);
    }
}

/*
 * Dispatch one voice macro line to its handler. Returns 1 if a macro
 * was consumed (caller should advance voiceIndex), 0 otherwise.
 */
static int dispatch_voice_macro(const char *trimmed, ToneData *td, ParseCtx *ctx)
{
    for (const VoiceMacro *m = kVoiceMacros; m->keyword; m++) {
        size_t klen = strlen(m->keyword);
        if (strncmp(trimmed, m->keyword, klen) != 0)
            continue;
        /* Must be followed by whitespace — prevents e.g. "voice_noise"
         * from matching "voice_noise_alt". */
        if (!isspace((unsigned char)trimmed[klen]))
            continue;
        const char *args = trimmed + klen + 1;
        while (*args && isspace((unsigned char)*args)) args++;

        switch (m->kind) {
            case MK_DIRECTSOUND:  handle_directsound(td,  m->voiceType, args, ctx); break;
            case MK_SQUARE_1:     handle_square_1(td,     m->voiceType, args, ctx); break;
            case MK_SQUARE_2:     handle_square_2(td,     m->voiceType, args, ctx); break;
            case MK_PROG_WAVE:    handle_prog_wave(td,    m->voiceType, args, ctx); break;
            case MK_NOISE:        handle_noise(td,        m->voiceType, args, ctx); break;
            case MK_KEYSPLIT:     handle_keysplit(td,     m->voiceType, args, ctx); break;
            case MK_KEYSPLIT_ALL: handle_keysplit_all(td, m->voiceType, args, ctx); break;
            case MK_CRY:          handle_cry(td,          m->voiceType, args, ctx); break;
        }
        return 1;
    }
    return 0;
}

/* ---- Top-level file parser ---- */

/*
 * Is this line the boundary between two voicegroups inside a
 * monolithic file? The ending is heuristic — ".align 2" is the
 * pokemerald convention, and any fresh "<label>::" starts a new one.
 */
static int is_monolithic_boundary(const char *trimmed)
{
    if (strncmp(trimmed, ".align", 6) == 0) return 1;
    const char *cc = strstr(trimmed, "::");
    if (cc && cc > trimmed && !isspace((unsigned char)trimmed[0])) return 1;
    return 0;
}

/*
 * Walk one .inc/.s file and populate ctx->vg->voices.
 *
 * If startLabel is NULL, we parse the whole file top to bottom
 * (per-voicegroup-file mode).
 *
 * If startLabel is non-NULL, we scan forward to a "<startLabel>::"
 * line and parse until the next label or .align directive.
 */
static int parse_voicegroup_file(const char *filePath, const char *startLabel, ParseCtx *ctx)
{
    vg_log("parse_voicegroup_file: '%s' label='%s'", filePath, startLabel ? startLabel : "(none)");

    FILE *f = fopen(filePath, "r");
    if (!f) {
        vg_err("cannot open %s", filePath);
        return -1;
    }

    char searchLabel[VG_MAX_SYMBOL_LEN + 4];
    if (startLabel)
        snprintf(searchLabel, sizeof(searchLabel), "%s::", startLabel);

    char line[VG_MAX_LINE];
    int voiceIndex = 0;
    int inSection = (startLabel == NULL);
    int voicesInSection = 0;

    while (fgets(line, sizeof(line), f) && voiceIndex < VOICEGROUP_SIZE) {
        char commentBuf[VG_MAX_VOICE_SAMPLE_NAME];
        commentBuf[0] = '\0';
        const char *at = strchr(line, '@');
        if (at) {
            const char *c = at + 1;
            while (*c && isspace((unsigned char)*c)) c++;
            size_t n = 0;
            while (*c && n + 1 < sizeof(commentBuf))
                commentBuf[n++] = *c++;
            while (n > 0 && isspace((unsigned char)commentBuf[n - 1])) n--;
            commentBuf[n] = '\0';
        }
        ctx->lineComment = commentBuf[0] ? commentBuf : NULL;

        vg_strip_comment(line);
        vg_rtrim(line);
        char *trimmed = vg_ltrim(line);
        if (trimmed[0] == '\0') continue;

        if (!inSection) {
            /* Skip until the requested label. */
            if (strstr(trimmed, searchLabel) == trimmed)
                inSection = 1;
            continue;
        }

        /* Once we've consumed voices, the next label/.align is the
         * next voicegroup. Stop. */
        if (startLabel && voicesInSection > 0 && is_monolithic_boundary(trimmed))
            break;

        /* voice_group <name>, <startingNote> shifts the slot index. */
        if (strncmp(trimmed, "voice_group ", 12) == 0) {
            char vgDeclName[VG_MAX_SYMBOL_LEN];
            int startingNote = 0;
            if (sscanf(trimmed + 12, "%[^,\n], %d", vgDeclName, &startingNote) >= 2 &&
                startingNote > 0 && startingNote < VOICEGROUP_SIZE) {
                voiceIndex = startingNote;
            }
            continue;
        }

        ToneData *td = &ctx->vg->voices[voiceIndex];
        if (dispatch_voice_macro(trimmed, td, ctx)) {
            voiceIndex++;
            voicesInSection++;
        }
    }

    vg_log("parse_voicegroup_file: done, voiceIndex=%d", voiceIndex);
    fclose(f);
    return 0;
}

/* ---- Public entry ---- */

int vg_parse_voicegroup(const char *projectRoot,
                        const char *voicegroupName,
                        LoadedVoiceGroup *vg,
                        const SymbolMap *dsMap,
                        const SymbolMap *pwMap,
                        const KeySplitMap *ksMap,
                        const ProjectDiscovery *disc)
{
    vg_log("vg_parse_voicegroup: searching for '%s'", voicegroupName);
    VoicegroupLocation loc = find_voicegroup(voicegroupName, disc);
    if (!loc.found) {
        vg_err("cannot find voicegroup '%s'", voicegroupName);
        return -1;
    }
    vg_log("vg_parse_voicegroup: found at '%s' label='%s'", loc.filePath, loc.label);

    /* Retain the source file path (only meaningful for emerald-style
     * per-file layouts — monolithic matches set loc.label). */
    if (!loc.label[0]) {
        strncpy(vg->sourceFile, loc.filePath, VG_MAX_PATH_LEN - 1);
        vg->sourceFile[VG_MAX_PATH_LEN - 1] = '\0';
    }

    WaveCache waveCache;
    vg_wave_cache_init(&waveCache);

    ParseCtx ctx = {
        .projectRoot = projectRoot,
        .dsMap = dsMap,
        .pwMap = pwMap,
        .ksMap = ksMap,
        .disc = disc,
        .vg = vg,
        .waveCache = &waveCache,
    };

    const char *startLabel = loc.label[0] ? loc.label : NULL;
    return parse_voicegroup_file(loc.filePath, startLabel, &ctx);
}

/* ---- Public helpers declared in voicegroup_loader.h ---- */
/* These stand-alone sample loaders are consumed by the project asset
 * override path. They share the registration helpers above so the
 * LoadedVoiceGroup owns the allocation. */

WaveData *voicegroup_loader_load_sample(const char *projectRoot,
                                        const char *relPath,
                                        LoadedVoiceGroup *vg)
{
    WaveData *wd = vg_load_sample(projectRoot, relPath);
    if (!wd) wd = vg_load_bin_sample(projectRoot, relPath);
    if (wd) register_wavedata(vg, wd);
    return wd;
}

uint32_t *voicegroup_loader_load_prog_wave(const char *projectRoot,
                                           const char *relPath,
                                           LoadedVoiceGroup *vg)
{
    uint32_t *pw = vg_load_prog_wave(projectRoot, relPath);
    if (pw) register_progwave(vg, pw);
    return pw;
}
