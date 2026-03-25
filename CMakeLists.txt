cmake_minimum_required(VERSION 3.24)
project(poryaaaa C CXX OBJC OBJCXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---- CLAP SDK ----
set(CLAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(clap-sdk)

# ---- clap-wrapper (standalone support) ----
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
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

set(PORYAAAA_APPLE_COMPILE_DEFINITIONS)
set(PORYAAAA_APPLE_COMPILE_OPTIONS)
if(APPLE)
    list(APPEND PORYAAAA_APPLE_COMPILE_DEFINITIONS GL_SILENCE_DEPRECATION=1)
    list(APPEND PORYAAAA_APPLE_COMPILE_OPTIONS
        -Wno-deprecated-declarations
        -Wno-gnu-statement-expression-from-macro-expansion
    )
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
        ${PUGL_DIR}/src/mac_gl.m
        ${PUGL_DIR}/src/mac_stub.m
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
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    plugin/imgui_impl_pugl.cpp
)

set(IMGUI_GLFW_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Common source files (shared between plugin and test)
set(ENGINE_SOURCES
    plugin/m4a_engine.c
    plugin/m4a_channel.c
    plugin/m4a_tables.c
    plugin/m4a_reverb.c
    plugin/voicegroup_loader.c
)

set(MIDI_TIMELINE_SOURCES
    plugin/midi_timeline.c
)

set(PORYAAAA_PLATFORM_DIALOG_SOURCES
    cmd/native_file_dialog_stub.cpp
)

if(APPLE)
    list(APPEND PORYAAAA_PLATFORM_DIALOG_SOURCES
        cmd/native_file_dialog_mac.mm
    )
endif()

set(PORYAAAA_PLUGIN_SOURCES
    plugin/m4a_plugin.c
    plugin/m4a_standalone_player.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${MIDI_TIMELINE_SOURCES}
    ${IMGUI_SOURCES}
    ${PUGL_SOURCES}
    ${PORYAAAA_PLATFORM_DIALOG_SOURCES}
)

set(PORYAAAA_PLUGIN_INCLUDE_DIRS
    plugin
    cmd
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PUGL_DIR}/include
)

# ---- CLAP Plugin ----
add_library(poryaaaa MODULE ${PORYAAAA_PLUGIN_SOURCES})

target_include_directories(poryaaaa PRIVATE ${PORYAAAA_PLUGIN_INCLUDE_DIRS})
target_compile_definitions(poryaaaa PRIVATE ${PORYAAAA_APPLE_COMPILE_DEFINITIONS})
target_compile_options(poryaaaa PRIVATE ${PORYAAAA_APPLE_COMPILE_OPTIONS})

# Build Pugl as a static library (not a DLL) on all platforms
target_compile_definitions(poryaaaa PRIVATE PUGL_STATIC)

# Use C++ linker (required because we mix C and C++ sources)
set_target_properties(poryaaaa PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    LINKER_LANGUAGE CXX
)

target_link_libraries(poryaaaa PRIVATE clap)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa PRIVATE m X11 Xcursor Xrandr Xext)
endif()

# OpenGL
if(APPLE)
    target_link_libraries(poryaaaa PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework CoreVideo")
elseif(WIN32)
    target_link_libraries(poryaaaa PRIVATE opengl32)
else()
    target_link_libraries(poryaaaa PRIVATE GL)
endif()

# Platform-specific settings
if(WIN32)
    target_link_libraries(poryaaaa PRIVATE dwmapi -static-libgcc -static-libstdc++)
elseif(APPLE)
    set_target_properties(poryaaaa PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Info.plist"
    )
endif()

# ---- VST3 Plugin ----
add_library(poryaaaa-vst3 MODULE ${PORYAAAA_PLUGIN_SOURCES})

target_include_directories(poryaaaa-vst3 PRIVATE ${PORYAAAA_PLUGIN_INCLUDE_DIRS})
target_compile_definitions(poryaaaa-vst3 PRIVATE PUGL_STATIC)
target_compile_definitions(poryaaaa-vst3 PRIVATE ${PORYAAAA_APPLE_COMPILE_DEFINITIONS})
target_compile_options(poryaaaa-vst3 PRIVATE ${PORYAAAA_APPLE_COMPILE_OPTIONS})

set_target_properties(poryaaaa-vst3 PROPERTIES
    LINKER_LANGUAGE CXX
)

target_link_libraries(poryaaaa-vst3 PRIVATE clap)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa-vst3 PRIVATE m X11 Xcursor Xrandr Xext)
endif()

if(APPLE)
    target_link_libraries(poryaaaa-vst3 PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework CoreVideo")
elseif(WIN32)
    target_link_libraries(poryaaaa-vst3 PRIVATE opengl32)
else()
    target_link_libraries(poryaaaa-vst3 PRIVATE GL)
endif()

if(WIN32)
    target_link_libraries(poryaaaa-vst3 PRIVATE dwmapi -static-libgcc -static-libstdc++)
endif()

target_add_vst3_wrapper(
    TARGET poryaaaa-vst3
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

target_link_libraries(poryaaaa_test PRIVATE m)

# ---- Standalone Renderer ----
add_executable(poryaaaa_render
    cmd/poryaaaa_render.c
    ${ENGINE_SOURCES}
    ${MIDI_TIMELINE_SOURCES}
)
target_include_directories(poryaaaa_render PRIVATE plugin third_party)
target_link_libraries(poryaaaa_render PRIVATE m)
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

target_link_libraries(poryaaaa_unit_tests PRIVATE m)

# ---- Standalone (clap-wrapper) ----
add_executable(poryaaaa-standalone ${PORYAAAA_PLUGIN_SOURCES})
target_include_directories(poryaaaa-standalone PRIVATE ${PORYAAAA_PLUGIN_INCLUDE_DIRS})
target_compile_definitions(poryaaaa-standalone PRIVATE ${PORYAAAA_APPLE_COMPILE_DEFINITIONS})
target_compile_definitions(poryaaaa-standalone PRIVATE PORYAAAA_STANDALONE_APP=1)
target_compile_options(poryaaaa-standalone PRIVATE ${PORYAAAA_APPLE_COMPILE_OPTIONS})
set_target_properties(poryaaaa-standalone PROPERTIES
    LINKER_LANGUAGE CXX
)

# Build Pugl as a static library (not a DLL) on all platforms
target_compile_definitions(poryaaaa-standalone PRIVATE PUGL_STATIC)
target_link_libraries(poryaaaa-standalone PRIVATE clap)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE m GL X11 Xcursor Xrandr Xext)
elseif(APPLE)
    target_link_libraries(poryaaaa-standalone PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework CoreVideo")
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
