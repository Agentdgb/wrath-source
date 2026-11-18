cmake_minimum_required(VERSION 3.20)
project(BypassWrath LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC x64 only
if(NOT MSVC)
    message(FATAL_ERROR "MSVC required (Visual Studio 2022, x64)")
endif()

# Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Global MSVC flags
add_compile_options(
    /W3
    /MP             # parallel compile
    /GS-            # disable buffer security checks (reduces overhead in PIC stubs)
    /Gy             # function-level linking
    /GL             # whole-program optimization
    /O2
    /Oi             # intrinsics
)
add_link_options(
    /LTCG           # link-time code gen (matches /GL)
    /OPT:REF        # strip unreferenced functions
    /OPT:ICF        # merge identical COMDATs
)

add_subdirectory(loader)
add_subdirectory(payload)
