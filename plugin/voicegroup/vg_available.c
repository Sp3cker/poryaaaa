#include "vg_available.h"
#include "vg_discovery.h"
#include "vg_symbols.h"
#include "vg_paths.h"
#include "vg_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define AVAIL_INITIAL_CAPACITY 128

/* ---- Small dynamic string-set for deduping symbols ---- */

typedef struct {
    char (*items)[VG_MAX_SYMBOL_LEN];
    int count;
    int capacity;
} StringSet;

static void set_init(StringSet *s)
{
    s->items = NULL;
    s->count = 0;
    s->capacity = 0;
}

static void set_free(StringSet *s)
{
    free(s->items);
    s->items = NULL;
    s->count = s->capacity = 0;
}

static int set_contains(const StringSet *s, const char *sym)
{
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->items[i], sym) == 0) return 1;
    return 0;
}

static void set_add(StringSet *s, const char *sym)
{
    if (set_contains(s, sym)) return;
    if (s->count >= s->capacity) {
        int nc = s->capacity ? s->capacity * 2 : 32;
        s->items = realloc(s->items, sizeof(s->items[0]) * nc);
        s->capacity = nc;
    }
    strncpy(s->items[s->count], sym, VG_MAX_SYMBOL_LEN - 1);
    s->items[s->count][VG_MAX_SYMBOL_LEN - 1] = '\0';
    s->count++;
}

/* ---- Keysplit pair set (sub_vg, ks_table, kind) ---- */

typedef struct {
    char subVg[VG_MAX_SYMBOL_LEN];
    char ksTable[VG_MAX_SYMBOL_LEN];
    int isAll; /* voice_keysplit_all has no ks_table */
} KeysplitPair;

typedef struct {
    KeysplitPair *items;
    int count;
    int capacity;
} KeysplitPairSet;

static void kpset_init(KeysplitPairSet *s)
{
    s->items = NULL;
    s->count = 0;
    s->capacity = 0;
}

static void kpset_free(KeysplitPairSet *s)
{
    free(s->items);
    s->items = NULL;
    s->count = s->capacity = 0;
}

static int kpset_contains(const KeysplitPairSet *s, const KeysplitPair *k)
{
    for (int i = 0; i < s->count; i++) {
        if (s->items[i].isAll != k->isAll) continue;
        if (strcmp(s->items[i].subVg, k->subVg) != 0) continue;
        if (strcmp(s->items[i].ksTable, k->ksTable) != 0) continue;
        return 1;
    }
    return 0;
}

static void kpset_add(KeysplitPairSet *s, const KeysplitPair *k)
{
    if (kpset_contains(s, k)) return;
    if (s->count >= s->capacity) {
        int nc = s->capacity ? s->capacity * 2 : 16;
        s->items = realloc(s->items, sizeof(KeysplitPair) * nc);
        s->capacity = nc;
    }
    s->items[s->count++] = *k;
}

/* ---- Output list ---- */

void vg_available_init(AvailableInstrumentList *list)
{
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

void vg_available_free(AvailableInstrumentList *list)
{
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void avail_add(AvailableInstrumentList *list, const char *name, const char *macro)
{
    if (list->count >= list->capacity) {
        int nc = list->capacity ? list->capacity * 2 : AVAIL_INITIAL_CAPACITY;
        list->entries = realloc(list->entries, sizeof(AvailableInstrument) * nc);
        list->capacity = nc;
    }
    AvailableInstrument *e = &list->entries[list->count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    strncpy(e->macro, macro, sizeof(e->macro) - 1);
    e->macro[sizeof(e->macro) - 1] = '\0';
}

/* ---- Helpers for line scanning ---- */

/*
 * Parse a `voice_directsound <key>, <pan>, <sample_sym>, ...` line.
 * Writes the sample symbol into outSym (capped at VG_MAX_SYMBOL_LEN).
 * Returns 1 on success, 0 if the line does not match the expected shape.
 */
static int parse_directsound_line(const char *trimmed, char *outSym)
{
    /* Must start with "voice_directsound" and a delimiter (space/tab). */
    const char *prefix = "voice_directsound";
    size_t plen = strlen(prefix);
    if (strncmp(trimmed, prefix, plen) != 0) return 0;
    char c = trimmed[plen];
    /* Accept voice_directsound / voice_directsound_no_resample / _alt. */
    if (c != ' ' && c != '\t' && c != '_') return 0;
    /* Skip through the macro name + whitespace. */
    const char *p = trimmed + plen;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    /* key, pan, sample_sym, ... — advance past two commas. */
    int commas = 0;
    while (*p && commas < 2) {
        if (*p == ',') commas++;
        p++;
    }
    if (commas < 2) return 0;
    while (*p == ' ' || *p == '\t') p++;
    /* Read symbol. */
    int i = 0;
    while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n' && i < VG_MAX_SYMBOL_LEN - 1)
        outSym[i++] = *p++;
    outSym[i] = '\0';
    return i > 0;
}

/*
 * Parse a `voice_keysplit[_all] <sub_vg>, [<ks_table>]` line.
 * Writes sub_vg and ks_table into outputs; sets *outIsAll.
 * Returns 1 on match.
 */
static int parse_keysplit_line(const char *trimmed, char *outSub, char *outTable, int *outIsAll)
{
    const char *p;
    int isAll;
    if (strncmp(trimmed, "voice_keysplit_all", 18) == 0
        && (trimmed[18] == ' ' || trimmed[18] == '\t')) {
        isAll = 1;
        p = trimmed + 18;
    } else if (strncmp(trimmed, "voice_keysplit", 14) == 0
               && (trimmed[14] == ' ' || trimmed[14] == '\t')) {
        isAll = 0;
        p = trimmed + 14;
    } else {
        return 0;
    }
    while (*p == ' ' || *p == '\t') p++;
    int i = 0;
    while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n' && i < VG_MAX_SYMBOL_LEN - 1)
        outSub[i++] = *p++;
    outSub[i] = '\0';
    outTable[0] = '\0';
    if (!isAll) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        int j = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ',' && j < VG_MAX_SYMBOL_LEN - 1)
            outTable[j++] = *p++;
        outTable[j] = '\0';
        if (j == 0) return 0;
    }
    *outIsAll = isAll;
    return i > 0;
}

/* ---- Voicegroup-file scanning ---- */

/*
 * A voicegroup file is treated as a "keysplit sub-voicegroup" when its
 * parent directory's last component is "keysplits" or "drumsets", OR
 * the filename (stem) ends in _keysplit / _drumset. This is the
 * pokeemerald convention the parser already relies on.
 */
static int path_parent_is(const char *filePath, const char *dirName)
{
    /* Find the last separator, then the one before it. */
    const char *last = NULL;
    const char *prev = NULL;
    for (const char *p = filePath; *p; p++) {
        if (*p == '/' || *p == '\\') {
            prev = last;
            last = p;
        }
    }
    if (!last || !prev) return 0;
    size_t len = (size_t)(last - prev - 1);
    size_t dnlen = strlen(dirName);
    if (len != dnlen) return 0;
    return strncmp(prev + 1, dirName, dnlen) == 0;
}

static int stem_endswith(const char *filePath, const char *suffix)
{
    const char *base = vg_path_basename(filePath);
    if (!base) return 0;
    const char *dot = strrchr(base, '.');
    size_t stemLen = dot ? (size_t)(dot - base) : strlen(base);
    size_t sLen = strlen(suffix);
    if (stemLen < sLen) return 0;
    return strncmp(base + stemLen - sLen, suffix, sLen) == 0;
}

static int is_keysplit_sub_vgfile(const char *filePath)
{
    if (path_parent_is(filePath, "keysplits")) return 1;
    if (path_parent_is(filePath, "drumsets")) return 1;
    if (stem_endswith(filePath, "_keysplit")) return 1;
    if (stem_endswith(filePath, "_drumset")) return 1;
    return 0;
}

/*
 * Scan one voicegroup file. Accumulates:
 *   - sample symbols used, tagged with whether the containing file
 *     is a keysplit sub-voicegroup
 *   - unique (sub_vg, ks_table) keysplit pairs
 */
static void scan_voicegroup_file(const char *filePath,
                                 StringSet *standaloneSamples,
                                 StringSet *ksUseSamples,
                                 KeysplitPairSet *ksPairs)
{
    FILE *f = fopen(filePath, "r");
    if (!f) return;
    int isSub = is_keysplit_sub_vgfile(filePath);
    char line[VG_MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        vg_strip_comment(line);
        char *trimmed = vg_ltrim(line);
        vg_rtrim(trimmed);
        if (!trimmed[0]) continue;

        char sym[VG_MAX_SYMBOL_LEN];
        if (parse_directsound_line(trimmed, sym)) {
            if (isSub) set_add(ksUseSamples, sym);
            else       set_add(standaloneSamples, sym);
            continue;
        }

        char sub[VG_MAX_SYMBOL_LEN];
        char tbl[VG_MAX_SYMBOL_LEN];
        int isAll = 0;
        if (parse_keysplit_line(trimmed, sub, tbl, &isAll)) {
            KeysplitPair pair;
            memset(&pair, 0, sizeof(pair));
            strncpy(pair.subVg, sub, sizeof(pair.subVg) - 1);
            strncpy(pair.ksTable, tbl, sizeof(pair.ksTable) - 1);
            pair.isAll = isAll;
            kpset_add(ksPairs, &pair);
        }
    }
    fclose(f);
}

static void scan_dir_for_voicegroups(const char *dirPath,
                                     StringSet *standaloneSamples,
                                     StringSet *ksUseSamples,
                                     KeysplitPairSet *ksPairs)
{
    DIR *d = opendir(dirPath);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!vg_str_ends_with_ci(ent->d_name, ".inc")
            && !vg_str_ends_with_ci(ent->d_name, ".s"))
            continue;
        char full[VG_MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s%c%s", dirPath, VG_PATH_SEP, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        scan_voicegroup_file(full, standaloneSamples, ksUseSamples, ksPairs);
    }
    closedir(d);
}

/* ---- Display-name helpers ---- */

static void sample_display_name(const char *symbol, const char *filePath,
                                char *out, size_t outSize)
{
    /* Prefer the sample's file basename (without extension). Fall back to
     * the symbol stripped of the canonical "DirectSoundWaveData_" prefix. */
    const char *base = filePath && filePath[0] ? vg_path_basename(filePath) : NULL;
    if (base) {
        const char *dot = strrchr(base, '.');
        size_t len = dot ? (size_t)(dot - base) : strlen(base);
        if (len >= outSize) len = outSize - 1;
        memcpy(out, base, len);
        out[len] = '\0';
        if (out[0]) return;
    }
    const char *prefix = "DirectSoundWaveData_";
    size_t plen = strlen(prefix);
    const char *src = (strncmp(symbol, prefix, plen) == 0) ? symbol + plen : symbol;
    strncpy(out, src, outSize - 1);
    out[outSize - 1] = '\0';
}

static void keysplit_display_name(const KeysplitPair *p, char *out, size_t outSize)
{
    const char *prefKS = "keysplit_table_";
    size_t pksLen = strlen(prefKS);
    const char *name = p->ksTable[0] ? p->ksTable : p->subVg;
    const char *src = (strncmp(name, prefKS, pksLen) == 0) ? name + pksLen : name;
    const char *suffix = p->isAll ? " (drumset)" : " (keysplit)";
    snprintf(out, outSize, "%s%s", src, suffix);
}

/* ---- Public entry ---- */

void vg_available_build(const char *projectRoot,
                        const VoicegroupLoaderConfig *config,
                        AvailableInstrumentList *out)
{
    vg_available_free(out);
    vg_available_init(out);
    if (!projectRoot || !projectRoot[0]) return;

    ProjectDiscovery disc;
    memset(&disc, 0, sizeof(disc));
    vg_discover_project(projectRoot, config, &disc);

    SymbolMap dsMap;
    vg_symbol_map_init(&dsMap);
    vg_parse_direct_sound_data(&disc, &dsMap);

    StringSet standaloneSamples;
    StringSet ksUseSamples;
    KeysplitPairSet ksPairs;
    set_init(&standaloneSamples);
    set_init(&ksUseSamples);
    kpset_init(&ksPairs);

    for (int i = 0; i < disc.voicegroupDirs.count; i++)
        scan_dir_for_voicegroups(disc.voicegroupDirs.paths[i],
                                 &standaloneSamples, &ksUseSamples, &ksPairs);

    /* Emit samples: include every known DirectSound sample unless it is
     * referenced ONLY from keysplit sub-voicegroups. Per-species Pokémon
     * cries are already excluded by the DirectSound symbol parser, which
     * stops on the `.if P_CRIES_ENABLED == TRUE` gate. */
    for (int i = 0; i < dsMap.count; i++) {
        const char *sym = dsMap.entries[i].symbol;
        int usedStand = set_contains(&standaloneSamples, sym);
        int usedInKS  = set_contains(&ksUseSamples, sym);
        if (!usedStand && usedInKS) continue;  /* hidden — keysplit represents it */

        char name[128];
        sample_display_name(sym, dsMap.entries[i].filePath, name, sizeof(name));
        char macro[384];
        snprintf(macro, sizeof(macro),
                 "\tvoice_directsound 60, 0, %s, 255, 180, 20, 150", sym);
        avail_add(out, name, macro);
    }

    /* Emit keysplits / drumsets. */
    for (int i = 0; i < ksPairs.count; i++) {
        const KeysplitPair *p = &ksPairs.items[i];
        char name[128];
        keysplit_display_name(p, name, sizeof(name));
        char macro[384];
        if (p->isAll) {
            snprintf(macro, sizeof(macro), "\tvoice_keysplit_all %s", p->subVg);
        } else {
            snprintf(macro, sizeof(macro), "\tvoice_keysplit %s, %s",
                     p->subVg, p->ksTable);
        }
        avail_add(out, name, macro);
    }

    vg_log("vg_available_build: %d entries (%d samples + %d keysplits)",
           out->count, dsMap.count, ksPairs.count);

    set_free(&standaloneSamples);
    set_free(&ksUseSamples);
    kpset_free(&ksPairs);
    vg_symbol_map_free(&dsMap);
}
