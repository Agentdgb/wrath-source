cmake_minimum_required(VERSION 3.20)
project(BypassWrath LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "MSVC required (Visual Studio 2022+, x64)")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Global flags: optimization + parallel compile.
# /GS- and /GL are set per-target to avoid CRT init conflicts with /MT.
add_compile_options(
    /W3
    /MP
    /Gy             # function-level linking (needed for /OPT:REF)
    /O2
    /Oi
)
add_link_options(
    /OPT:REF
    /OPT:ICF
)

add_subdirectory(loader)
add_subdirectory(payload)
add_subdirectory(test_payload)
