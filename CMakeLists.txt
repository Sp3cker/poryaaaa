cmake_minimum_required(VERSION 3.24)
project(poryaaaa C CXX OBJC OBJCXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---- CLAP SDK ----
set(CLAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(clap-sdk)

# ---- clap-wrapper (VST3 + standalone support) ----
# Apply vendored clap-wrapper patches idempotently. The patch adds a
# VST3 kLegacyMIDICCOutEvent -> CLAP_EVENT_MIDI input-side translation
# that upstream clap-wrapper does not yet ship; see patches/README.
# Marker `kLegacyMIDICCOutEvent` appears only after the patch is applied.
set(_poryaaaa_wrapper_proc
    "${CMAKE_CURRENT_SOURCE_DIR}/clap-wrapper/src/detail/vst3/process.cpp")
if(EXISTS "${_poryaaaa_wrapper_proc}")
    file(READ "${_poryaaaa_wrapper_proc}" _poryaaaa_wrapper_content)
    if(NOT _poryaaaa_wrapper_content MATCHES "kLegacyMIDICCOutEvent")
        execute_process(
            COMMAND git apply --verbose
                    "${CMAKE_CURRENT_SOURCE_DIR}/patches/clap-wrapper-midi-event-bridge.patch"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/clap-wrapper"
            RESULT_VARIABLE _poryaaaa_patch_result
            ERROR_VARIABLE  _poryaaaa_patch_err)
        if(NOT _poryaaaa_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply clap-wrapper patch: ${_poryaaaa_patch_err}")
        endif()
        message(STATUS "poryaaaa: applied clap-wrapper MIDI-event-bridge patch")
    endif()
endif()

set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
set(CLAP_WRAPPER_COPY_AFTER_BUILD TRUE CACHE BOOL "" FORCE)
add_subdirectory(clap-wrapper)
# clap-wrapper enables -Werror but its own fsutil.cpp triggers -Wreturn-type
# with MinGW cross-compilation, so relax that specific warning.
# Also ensure _WIN32_WINNT targets Vista+ for SHGetKnownFolderPath.
if(TARGET clap-wrapper-compile-options)
    target_compile_options(clap-wrapper-compile-options INTERFACE -Wno-error=return-type)
    if(WIN32 AND MINGW)
        target_compile_definitions(clap-wrapper-compile-options INTERFACE _WIN32_WINNT=0x0600)
    endif()
endif()

# ---- Pugl ----
set(PUGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugl)

# Platform-specific Pugl sources
if(WIN32)
    set(PUGL_PLATFORM_SOURCES
        ${PUGL_DIR}/src/win.c
        ${PUGL_DIR}/src/win_gl.c
        ${PUGL_DIR}/src/win_stub.c
    )
elseif(APPLE)
    set(PUGL_PLATFORM_SOURCES
        ${PUGL_DIR}/src/mac.m
        ${PUGL_DIR}/src/mac_stub.m
        plugin/metal/pugl_mac_metal.m
    )
else()
    set(PUGL_PLATFORM_SOURCES
        ${PUGL_DIR}/src/x11.c
        ${PUGL_DIR}/src/x11_gl.c
        ${PUGL_DIR}/src/x11_stub.c
    )
endif()

set(PUGL_SOURCES
    ${PUGL_DIR}/src/common.c
    ${PUGL_DIR}/src/internal.c
    ${PUGL_PLATFORM_SOURCES}
)

# ---- Dear ImGui ----
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    plugin/imgui_impl_pugl.cpp
)
if(APPLE)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_metal.mm)
else()
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
endif()

# ---- Voicegroup loader (decomp parser, reusable by external tools) ----
# Pure C, no audio-engine dependencies. Defined in its own
# CMakeLists so external projects (ccomidi etc.) can add_subdirectory
# just the library without dragging in poryaaaa's plugin /
# standalone / test targets.
add_subdirectory(plugin/voicegroup)

# m4a audio engine sources (shared between plugin, standalone, tests).
set(ENGINE_SOURCES
    plugin/m4a_engine.c
    plugin/m4a_channel.c
    plugin/m4a_tables.c
    plugin/m4a_reverb.c
)

# ---- CLAP Plugin ----
add_library(poryaaaa MODULE
    plugin/m4a_plugin.c
    plugin/m4a_params.c
    plugin/voicegroup/project_asset_index.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${PUGL_SOURCES}
)

target_include_directories(poryaaaa PRIVATE
    plugin
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PUGL_DIR}/include
    ${PUGL_DIR}/src
)

# Build Pugl as a static library (not a DLL) on all platforms
target_compile_definitions(poryaaaa PRIVATE PUGL_STATIC)

# On macOS, compile m4a_gui.cpp as Objective-C++ so it can use Metal APIs
if(APPLE)
    set_source_files_properties(plugin/m4a_gui.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

# Use C++ linker (required because we mix C and C++ sources)
set_target_properties(poryaaaa PROPERTIES
    LINKER_LANGUAGE CXX
)

target_link_libraries(poryaaaa PRIVATE clap voicegroup_loader)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa PRIVATE m X11 Xcursor Xrandr Xext)
endif()

# Graphics
if(APPLE)
    target_link_libraries(poryaaaa PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
elseif(WIN32)
    target_link_libraries(poryaaaa PRIVATE opengl32)
else()
    target_link_libraries(poryaaaa PRIVATE GL)
endif()

# Platform-specific settings
if(WIN32)
    target_link_libraries(poryaaaa PRIVATE dwmapi -static-libgcc -static-libstdc++)
endif()

target_add_vst3_wrapper(
    TARGET poryaaaa
    OUTPUT_NAME "poryaaaa"
)

# ---- Standalone Test (WAV export) ----
add_executable(poryaaaa_test
    test/test_wav_export.c
    ${ENGINE_SOURCES}
)

target_include_directories(poryaaaa_test PRIVATE
    plugin
)

target_link_libraries(poryaaaa_test PRIVATE m voicegroup_loader)

# ---- Standalone Renderer ----
add_executable(poryaaaa_render
    cmd/poryaaaa_render.c
    ${ENGINE_SOURCES}
)
target_include_directories(poryaaaa_render PRIVATE plugin third_party)
target_link_libraries(poryaaaa_render PRIVATE m voicegroup_loader)
if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa_render PRIVATE pthread dl)
endif()
if(APPLE)
    target_link_libraries(poryaaaa_render PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
endif()
if(WIN32)
    target_link_libraries(poryaaaa_render PRIVATE -static-libgcc)
endif()

# ---- Unit Tests ----
add_executable(poryaaaa_unit_tests
    test/test_engine.c
    ${ENGINE_SOURCES}
)

target_include_directories(poryaaaa_unit_tests PRIVATE
    plugin
)

target_link_libraries(poryaaaa_unit_tests PRIVATE m voicegroup_loader)

# ---- Standalone (clap-wrapper) ----
add_executable(poryaaaa-standalone
    plugin/m4a_plugin.c
    plugin/m4a_params.c
    plugin/voicegroup/project_asset_index.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${PUGL_SOURCES}
)
target_include_directories(poryaaaa-standalone PRIVATE
    plugin
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PUGL_DIR}/include
    ${PUGL_DIR}/src
)
set_target_properties(poryaaaa-standalone PROPERTIES
    LINKER_LANGUAGE CXX
)

# Build Pugl as a static library (not a DLL) on all platforms
target_compile_definitions(poryaaaa-standalone PRIVATE PUGL_STATIC)
target_link_libraries(poryaaaa-standalone PRIVATE clap voicegroup_loader)

# On macOS, compile m4a_gui.cpp as Objective-C++ so it can use Metal APIs
if(APPLE)
    set_source_files_properties(plugin/m4a_gui.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE m GL X11 Xcursor Xrandr Xext)
elseif(APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
elseif(WIN32)
    target_link_libraries(poryaaaa-standalone PRIVATE opengl32 dwmapi -static-libgcc -static-libstdc++)
endif()

target_add_standalone_wrapper(
    TARGET poryaaaa-standalone
    OUTPUT_NAME "poryaaaa_standalone"
    STATICALLY_LINKED_CLAP_ENTRY TRUE
    PLUGIN_ID "com.huderlem.poryaaaa"
)
set_target_properties(poryaaaa-standalone PROPERTIES OUTPUT_NAME "poryaaaa_standalone")

# clap-wrapper's native Win32 standalone requires WinRT/WIL (MSVC/Clang only).
# For MinGW, replace the generic wrapasstandalone.cpp with our own lightweight
# Win32 entry point that handles GUI hosting and timer firing.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_target_property(_sa_sources poryaaaa-standalone SOURCES)
    list(FILTER _sa_sources EXCLUDE REGEX "wrapasstandalone\\.cpp$")
    set_target_properties(poryaaaa-standalone PROPERTIES SOURCES "${_sa_sources}")
    target_sources(poryaaaa-standalone PRIVATE plugin/standalone_main_win32.cpp)
    target_include_directories(poryaaaa-standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/clap-wrapper/src
    )
endif()

if(UNIX AND NOT APPLE)
    get_target_property(_sa_sources_lin poryaaaa-standalone SOURCES)
    list(FILTER _sa_sources_lin EXCLUDE REGEX "wrapasstandalone\\.cpp$")
    set_target_properties(poryaaaa-standalone PROPERTIES SOURCES "${_sa_sources_lin}")
    target_sources(poryaaaa-standalone PRIVATE plugin/standalone_main_linux.cpp)
    target_include_directories(poryaaaa-standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/clap-wrapper/src
    )

    # Our standalone uses Pugl (not GTK), so strip GTK3 from the
    # clap-wrapper standalone library to prevent assert(gtkGui) crash
    # when gtkmm-3.0 happens to be installed on the build system.
    set(_salib poryaaaa-standalone-clap-wrapper-standalone-lib)
    get_target_property(_salib_sources ${_salib} SOURCES)
    if(_salib_sources)
        list(FILTER _salib_sources EXCLUDE REGEX "gtkutils\\.cpp$")
        set_target_properties(${_salib} PROPERTIES SOURCES "${_salib_sources}")
    endif()
    foreach(_prop COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
        get_target_property(_defs ${_salib} ${_prop})
        if(_defs)
            list(REMOVE_ITEM _defs "CLAP_WRAPPER_HAS_GTK3")
            set_target_properties(${_salib} PROPERTIES ${_prop} "${_defs}")
        endif()
    endforeach()
endif()
