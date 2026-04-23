# voicegroup_loader — pure-C parser for pokeemerald/pokefirered
# voicegroup files. No audio-engine dependencies; any tool that wants
# to read a decomp project's voices can link this library.

add_library(voicegroup_loader STATIC
    voicegroup_loader.c
    vg_log.c
    vg_paths.c
    vg_discovery.c
    vg_symbols.c
    vg_keysplit.c
    vg_wav.c
    vg_parser.c
)

# PUBLIC so consumers pick up the "voicegroup/xxx.h" prefix we use
# throughout the project.
target_include_directories(voicegroup_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
