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

set_target_properties(bypass_payload PROPERTIES
    OUTPUT_NAME "bypass_payload"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    # /GS- already global; additionally strip debug info from release
)

# DEF file sets LIBRARY name to "javaw" → EAT DllName = "javaw"
# so a scanner reading IMAGE_EXPORT_DIRECTORY->Name sees "javaw"
set_target_properties(bypass_payload PROPERTIES
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/bypass_payload.def\""
)

target_link_options(bypass_payload PRIVATE /SUBSYSTEM:WINDOWS)

# Post-build: scrub RTTI namespace strings from the DLL.
# scrub_rtti.py zeroes any null-terminated string in .rdata that starts with
# the payload's C++ namespace prefix (e.g. "bp::") -- removes readable
# type descriptor strings that screenshare scanners grep for.
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
