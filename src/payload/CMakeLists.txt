cmake_minimum_required(VERSION 3.24)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(WrathClient
    VERSION 0.1.0
    DESCRIPTION "Full C++ Minecraft utility client for 1.21.0-1.21.11 (Vanilla + Fabric)"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unified output: vantaclient.dll AND vanta_injector.exe both land in build/dist/
# regardless of Debug/Release. For multi-config generators (Visual Studio), MSVC
# normally appends the config as a subdir â€” the per-config overrides below
# force them all to the same folder so distribution is one directory.
set(VANTA_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${VANTA_DIST_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${VANTA_DIST_DIR}")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${VANTA_DIST_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${VANTA_DIST_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(VANTA_REGEN_MAPS   "Regenerate mappings blobs from Internet (requires Python 3.10+)" OFF)
option(VANTA_BUILD_TESTS  "Build unit tests"                                                 OFF)
option(VANTA_WITH_INJECTOR "Build the native LoadLibrary injector side target"                ON)
option(VANTA_STATIC_CRT   "Link static CRT so no VC redist is needed on the target machine"   ON)

if (MSVC)
    add_compile_options(
        /W4 /permissive- /Zc:__cplusplus /EHsc /MP /utf-8
        /wd4100 /wd4127 /wd4201 /wd4324
    )
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
        UNICODE _UNICODE
    )
    add_link_options($<$<CONFIG:Debug>:/DEBUG:FULL>)
    if (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
        add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw> $<$<CONFIG:Release>:/GR-> $<$<CONFIG:Release>:/GL>)
        add_link_options($<$<CONFIG:Release>:/DEBUG:NONE> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    endif()
endif()

include(FindJvmHeaders)
include(FetchDeps)
include(EmbedBinary)

# â”€â”€ vendored ImGui â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
add_library(vanta_imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_opengl3.cpp   # we draw inside Minecraft's GL frame
    vendor/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(vanta_imgui PUBLIC
    vendor/imgui
    vendor/imgui/backends
)
target_compile_definitions(vanta_imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)
set_target_properties(vanta_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# â”€â”€ vendored jnihook (native Java-method detour lib the triggerbot uses) â”€â”€â”€â”€â”€
# Self-contained: ClassFileLoadHook + RegisterNatives, no external deps beyond
# jni.h/jvmti.h which come from FindJvmHeaders. Its own CMakeLists in the
# upstream tree is not vendored â€” we just compile the .cpp files directly.
add_library(vanta_jnihook STATIC
    vendor/jnihook/src/jnihook.cpp
    vendor/jnihook/src/classfile.cpp
    vendor/jnihook/src/uuid.cpp
    vendor/jnihook/src/hook_tls_stub.cpp   # no-op resolver for the TLS-wire callback
)
target_include_directories(vanta_jnihook PUBLIC
    vendor/jnihook/include
    ${VANTA_JVM_INCLUDE_DIRS}
)
target_include_directories(vanta_jnihook PRIVATE vendor/jnihook/src src)
set_target_properties(vanta_jnihook PROPERTIES POSITION_INDEPENDENT_CODE ON)

# â”€â”€ main DLL â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
file(GLOB_RECURSE VANTA_SRC CONFIGURE_DEPENDS
    src/entry/*.cpp
    src/platform/*.cpp
    src/util/*.cpp
    src/jvm/*.cpp
    src/mappings/*.cpp
    src/mappings/generated/*.cpp
    src/asm/*.cpp
    src/hook/*.cpp
    src/hook/bridge/*.cpp
    src/hook/recipes/*.cpp
    src/hook/sites/*.cpp
    src/core/*.cpp
    src/core/**/*.cpp
    src/modules/**/*.cpp
    src/generated/*.cpp
    src/vgui/*.cc
    src/vgui/**/*.cc
)

add_library(display_service SHARED ${VANTA_SRC} src/entry/version.rc)
if(MSVC)
    set_source_files_properties(src/modules/combat/AimAssist.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
target_sources(display_service PRIVATE assets/bloom/bloom.rc)
target_sources(display_service PRIVATE
    src/modules/combat/aimassist/wind/aimassist_regular.cpp
    src/modules/combat/aimassist/wind/aimassist_cache.cpp
    src/modules/combat/aimassist/wind/aimassist_debug.cpp
    src/modules/combat/aimassist/wind/WindSupport.cpp
    src/modules/combat/aimassist/wind/Engine.cpp
)


target_include_directories(display_service PRIVATE
    src
    ${CMAKE_SOURCE_DIR}
    vendor
    vendor/json
    vendor/ankerl
    vendor/imgui                 # vgui uses ImGui via unqualified "imgui.h"
    vendor/imgui/backends
    vendor/jnihook/include       # the ported triggerbot's #include <jnihook.h> resolves here
    vendor/vanta_compat          # <sdk/mappings/mappings.hpp> for the ported triggerbot
    ${VANTA_JVM_INCLUDE_DIRS}
    ${minhook_SOURCE_DIR}/include  # MinHook's CMakeLists does not propagate its headers
)
option(VANTA_INPUT_TRACE "Log every click's logical coordinates (diagnostics)" OFF)
if (VANTA_INPUT_TRACE)
    target_compile_definitions(display_service PRIVATE VANTA_INPUT_TRACE)
endif()

target_link_libraries(display_service PRIVATE vanta_imgui minhook vanta_jnihook)
target_link_libraries(display_service PRIVATE
    opengl32 gdi32 user32 psapi advapi32 shell32 shlwapi ole32 dbghelp bcrypt
    imm32 dwmapi winhttp
    # Colour emoji: Direct2D draws COLR/CPAL glyphs into a WIC bitmap in system
    # memory (no D3D device — we render through OpenGL), which is then uploaded
    # as a GL texture. See vgui/vanta/render/emoji.cc.
    d2d1 dwrite windowscodecs
)

set_target_properties(display_service PROPERTIES
    OUTPUT_NAME "display_service64"
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(MSVC)
    target_link_options(display_service PRIVATE "/FIXED:NO")
endif()

# ── runtime assets (all fonts, SVGs, and PNGs are now embedded in .rdata) ───
add_custom_command(TARGET display_service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:display_service>/vanta_assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:display_service>"
            "${CMAKE_SOURCE_DIR}/../build_out/display_service64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:display_service>"
            "C:/Users/Admin/Injeccion/display_service64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:display_service>"
            "C:/Users/Admin/AppData/Local/UtilData/display_service64.dll"
    COMMENT "Deploying display_service64.dll to build_out, Injeccion, and UtilData"
)



if (VANTA_REGEN_MAPS)
    find_package(Python3 3.10 REQUIRED)
    add_custom_target(vanta_genmaps
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/genmaps/genmaps.py
                --out ${CMAKE_CURRENT_SOURCE_DIR}/src/mappings/generated
                --selectors ${CMAKE_CURRENT_SOURCE_DIR}/tools/genmaps/selectors/vanta.yml
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Regenerating display_service mapping blobs for 1.21.0-1.21.11..."
        USES_TERMINAL
    )
    add_dependencies(display_service vanta_genmaps)
endif()

if (VANTA_WITH_INJECTOR)
    add_subdirectory(injector)
endif()

if (VANTA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

install(TARGETS display_service
    LIBRARY DESTINATION bin
    RUNTIME DESTINATION bin
)

message(STATUS "WrathClient ${PROJECT_VERSION} configured (MSVC=${MSVC}, JDK=${VANTA_JAVA_HOME})")
