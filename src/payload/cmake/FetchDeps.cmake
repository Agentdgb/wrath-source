# FetchDeps.cmake — MinHook fetched (source only) and built as a local static
# library. Upstream MinHook has no root CMakeLists, so FetchContent_MakeAvailable
# by itself does nothing useful — we glob its sources and define the target
# ourselves.
get_filename_component(_DT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_VENDORED_MINHOOK "${_DT_ROOT}/vendor/minhook")

if (EXISTS "${_VENDORED_MINHOOK}/src/hook.c")
    set(minhook_SOURCE_DIR "${_VENDORED_MINHOOK}")
else()
    include(FetchContent)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG        v1.3.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(minhook)
endif()

# Sources (portable subset — enough for x64 SwapBuffers hooking).
set(_MH_SRC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
)

add_library(minhook STATIC ${_MH_SRC})
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
target_include_directories(minhook PRIVATE ${minhook_SOURCE_DIR}/src)
set_target_properties(minhook PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
    OUTPUT_NAME "minhook"
    ARCHIVE_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
)
if (MSVC)
    target_compile_options(minhook PRIVATE /wd4267 /wd4244 /wd4996)
endif()
