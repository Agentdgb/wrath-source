# toolchain-msvc.cmake — opt-in toolchain to pin static CRT + MSVC settings.
# Usage: cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-msvc.cmake

set(CMAKE_C_COMPILER cl)
set(CMAKE_CXX_COMPILER cl)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
