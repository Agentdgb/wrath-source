cmake_minimum_required(VERSION 3.20)

add_library(bypass_payload SHARED
    src/DllMain.cpp
    src/Payload.cpp
    src/PayloadExports.cpp
    bypass_payload.def
)

target_include_directories(bypass_payload PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/shared
)

target_link_libraries(bypass_payload PRIVATE
    kernel32
    user32
)

# /MT -- static CRT. No dependency on vcruntime140.dll / ucrtbase.dll.
# Required for a manually-mapped DLL: the CRT initializer runs from DllMain
# via _DllMainCRTStartup; it must be self-contained. With /MD the CRT DLL
# must already be loaded in the target process -- not guaranteed.
set_target_properties(bypass_payload PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    OUTPUT_NAME "bypass_payload"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# DEF sets LIBRARY name → "javaw" so IMAGE_EXPORT_DIRECTORY->Name = "javaw"
set_target_properties(bypass_payload PROPERTIES
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/bypass_payload.def\""
)

target_link_options(bypass_payload PRIVATE /SUBSYSTEM:WINDOWS)

# /EHa: async SEH + C++ exceptions. Required for __try/__except in DllMain/PayloadThread.
target_compile_options(bypass_payload PRIVATE /EHa)

# Post-build: scrub RTTI namespace strings (.rdata)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_command(TARGET bypass_payload POST_BUILD
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tools/scrub_rtti.py"
                "$<TARGET_FILE:bypass_payload>"
        COMMENT "Scrubbing RTTI strings from bypass_payload.dll"
        VERBATIM
    )
else()
    message(STATUS "Python3 not found -- skipping RTTI scrub (safe to ignore)")
endif()
