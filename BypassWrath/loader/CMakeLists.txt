cmake_minimum_required(VERSION 3.20)

add_executable(BypassLoader
    src/main.cpp
)

target_include_directories(BypassLoader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/shared
)

target_link_libraries(BypassLoader PRIVATE
    kernel32
    ntdll
    advapi32
)

set_target_properties(BypassLoader PROPERTIES
    OUTPUT_NAME "BypassLoader"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# SUBSYSTEM:CONSOLE so wprintf shows in cmd
target_link_options(BypassLoader PRIVATE /SUBSYSTEM:CONSOLE)
