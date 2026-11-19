cmake_minimum_required(VERSION 3.20)

add_library(test_payload SHARED
    src/TestPayload.cpp
    test_payload.def
)

target_include_directories(test_payload PRIVATE
    ${CMAKE_SOURCE_DIR}/shared
)

target_link_libraries(test_payload PRIVATE kernel32 user32)

# /MT -- same as the real payload. That's the point: test under identical CRT conditions.
set_target_properties(test_payload PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    OUTPUT_NAME "test_payload"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# DEF sets LIBRARY name → "javaw" (same as production payload)
set_target_properties(test_payload PROPERTIES
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/test_payload.def\""
)

target_link_options(test_payload PRIVATE /SUBSYSTEM:WINDOWS /GUARD:NO /DYNAMICBASE /FIXED:NO)

# EH support required for std::thread and try/catch in test 2/3
# /EHa = async SEH + C++ exceptions (matches __try/__except + throw interop)
# /guard:cf-: Disable Control Flow Guard
target_compile_options(test_payload PRIVATE /EHa /guard:cf-)
