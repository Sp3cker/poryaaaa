#include "vg_symbols.h"
#include "vg_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 64

void vg_symbol_map_init(SymbolMap *map)
{
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

void vg_symbol_map_free(SymbolMap *map)
{
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static void symbol_map_add(SymbolMap *map, const char *symbol, const char *path)
{
    if (map->count >= map->capacity) {
        map->capacity = map->capacity ? map->capacity * 2 : INITIAL_CAPACITY;
        map->entries = realloc(map->entries, sizeof(SymbolMapping) * map->capacity);
    }
    strncpy(map->entries[map->count].symbol, symbol, VG_MAX_SYMBOL_LEN - 1);
    map->entries[map->count].symbol[VG_MAX_SYMBOL_LEN - 1] = '\0';
    strncpy(map->entries[map->count].filePath, path, VG_MAX_PATH_LEN - 1);
    map->entries[map->count].filePath[VG_MAX_PATH_LEN - 1] = '\0';
    map->count++;
}

const char *vg_symbol_map_find(const SymbolMap *map, const char *symbol)
{
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].symbol, symbol) == 0)
            return map->entries[i].filePath;
    }
    return NULL;
}

/*
 * DirectSound and ProgrammableWave symbol files share the same
 * on-disk format — a series of `<label>::` lines each followed by a
 * `.incbin "relative/path"` line — so one parser handles both.
 */
static void parse_symbol_inc_file(const char *filePath, SymbolMap *map)
{
    FILE *f = fopen(filePath, "r");
    if (!f) {
        vg_err("cannot open %s", filePath);
        return;
    }

    char line[VG_MAX_LINE];
    char currentSymbol[VG_MAX_SYMBOL_LEN] = {0};

    while (fgets(line, sizeof(line), f)) {
        vg_strip_comment(line);
        vg_rtrim(line);
        char *trimmed = vg_ltrim(line);

        /* Pokeemerald gates per-species cries behind `.if P_CRIES_ENABLED
         * == TRUE`. Everything past that line is cry data — not useful as a
         * musical sample. Stop parsing the file so the cries never enter
         * the symbol map. Benign for files that don't use this gate. */
        if (strncmp(trimmed, ".if", 3) == 0
            && strstr(trimmed, "P_CRIES_ENABLED"))
            break;

        char *colonColon = strstr(trimmed, "::");
        if (colonColon && colonColon > trimmed) {
            *colonColon = '\0';
            strncpy(currentSymbol, trimmed, VG_MAX_SYMBOL_LEN - 1);
            currentSymbol[VG_MAX_SYMBOL_LEN - 1] = '\0';
            continue;
        }

        if (currentSymbol[0] && strstr(trimmed, ".incbin")) {
            char *quote1 = strchr(trimmed, '"');
            if (quote1) {
                quote1++;
                char *quote2 = strchr(quote1, '"');
                if (quote2) {
                    *quote2 = '\0';
                    symbol_map_add(map, currentSymbol, quote1);
                }
            }
            currentSymbol[0] = '\0';
        }
    }

    fclose(f);
}

void vg_parse_direct_sound_data(const ProjectDiscovery *disc, SymbolMap *map)
{
    for (int i = 0; i < disc->directSoundDataFiles.count; i++)
        parse_symbol_inc_file(disc->directSoundDataFiles.paths[i], map);
}

void vg_parse_prog_wave_data(const ProjectDiscovery *disc, SymbolMap *map)
{
    for (int i = 0; i < disc->progWaveDataFiles.count; i++)
        parse_symbol_inc_file(disc->progWaveDataFiles.paths[i], map);
}
