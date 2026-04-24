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

# Common source files (shared between plugin and test)
set(ENGINE_SOURCES
    plugin/m4a_engine.c
    plugin/m4a_channel.c
    plugin/m4a_tables.c
    plugin/m4a_reverb.c
    plugin/voicegroup_loader.c
)

# Full plugin source list, shared between the CLAP, VST3, and standalone
# targets. Every plugin-format target rebuilds from source because clap-wrapper
# adds format-specific export entries to the same translation unit set.
set(PORYAAAA_PLUGIN_SOURCES
    plugin/m4a_plugin.c
    plugin/m4a_params.c
    plugin/project_asset_index.c
    plugin/m4a_gui.cpp
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${PUGL_SOURCES}
)

# ---- CLAP Plugin ----
add_library(poryaaaa MODULE
    plugin/m4a_plugin.c
    plugin/m4a_params.c
    plugin/project_asset_index.c
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
    PREFIX ""
    SUFFIX ".clap"
    LINKER_LANGUAGE CXX
)

target_link_libraries(poryaaaa PRIVATE clap)

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
elseif(APPLE)
    set_target_properties(poryaaaa PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
    )
    # MACOSX_BUNDLE_INFO_PLIST only works for MACOSX_BUNDLE (app bundles).
    # For a generic BUNDLE (.clap), copy Info.plist manually.
    #
    # Ad-hoc sign the full bundle (not just the Mach-O). Without this,
    # hardened-runtime plugin scanners (Ableton's PluginScanner in particular)
    # refuse the bundle with "code has no resources but signature indicates
    # they must be present" and report the plugin as "not a plugin".
    add_custom_command(TARGET poryaaaa POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:poryaaaa>/Info.plist"
        COMMAND codesign -f -s - "$<TARGET_BUNDLE_DIR:poryaaaa>"
    )
endif()

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
add_executable(poryaaaa-standalone
    plugin/m4a_plugin.c
    plugin/m4a_params.c
    plugin/project_asset_index.c
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
target_link_libraries(poryaaaa-standalone PRIVATE clap)

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

# ---- VST3 (clap-wrapper) ----
# Re-bundle the same plugin sources under a separate MODULE target so the
# clap-wrapper VST3 entry point can coexist with the CLAP entry. The wrapper
# calls into our plugin through the `clap_entry` symbol provided by
# m4a_plugin.c, so this target has everything it needs in-tree without a
# separate static lib.
add_library(poryaaaa-vst3 MODULE
    ${PORYAAAA_PLUGIN_SOURCES}
)

target_include_directories(poryaaaa-vst3 PRIVATE
    plugin
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PUGL_DIR}/include
    ${PUGL_DIR}/src
)

target_compile_definitions(poryaaaa-vst3 PRIVATE PUGL_STATIC)

set_target_properties(poryaaaa-vst3 PROPERTIES
    LINKER_LANGUAGE CXX
)

target_link_libraries(poryaaaa-vst3 PRIVATE clap)

if(UNIX AND NOT APPLE)
    target_link_libraries(poryaaaa-vst3 PRIVATE m X11 Xcursor Xrandr Xext)
endif()

if(APPLE)
    target_link_libraries(poryaaaa-vst3 PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
elseif(WIN32)
    target_link_libraries(poryaaaa-vst3 PRIVATE opengl32 dwmapi -static-libgcc -static-libstdc++)
else()
    target_link_libraries(poryaaaa-vst3 PRIVATE GL)
endif()

target_add_vst3_wrapper(
    TARGET poryaaaa-vst3
    OUTPUT_NAME "poryaaaa"
    BUNDLE_IDENTIFIER "com.huderlem.poryaaaa.vst3"
    BUNDLE_VERSION "1.0"
)

# Install the built VST3 into the user's plugin folder so DAWs pick it up
# without a separate copy step. Mirrors the CLAP install for `poryaaaa`.
# The `codesign -f -s -` seals the bundle as ad-hoc; without it, hardened-
# runtime scanners (Ableton) reject the bundle as "not a plugin".
if(APPLE)
    set(VST3_INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
    add_custom_command(TARGET poryaaaa-vst3 POST_BUILD
        COMMAND codesign -f -s - "$<TARGET_BUNDLE_DIR:poryaaaa-vst3>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VST3_INSTALL_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${VST3_INSTALL_DIR}/poryaaaa.vst3"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_BUNDLE_DIR:poryaaaa-vst3>"
            "${VST3_INSTALL_DIR}/poryaaaa.vst3"
    )
endif()
