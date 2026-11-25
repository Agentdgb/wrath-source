cmake_minimum_required(VERSION 3.24)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(Wrath LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WRATH_ROOT "${CMAKE_SOURCE_DIR}")
set(WRATH_INCLUDE "${CMAKE_SOURCE_DIR}/src")
set(WRATH_DEPLOY_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Runtime deploy directory")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/dist")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/dist")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

# Single include path for the whole tree.
include_directories(${WRATH_INCLUDE})

option(WRATH_BUILD_TESTS "Build client regression tests" OFF)
if(WRATH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(WRATH_SCREENSHARE_PRESET "Enforce screenshare preset invariants" OFF)

# ---- trace (library used by the loader) -------------------------------------
file(GLOB WRATH_TRACE_SRC "${CMAKE_SOURCE_DIR}/src/trace/*.cpp")
add_library(wrath_trace STATIC ${WRATH_TRACE_SRC})
target_include_directories(wrath_trace PUBLIC ${WRATH_INCLUDE})
if(MSVC)
    # C4201: anonymous structs are intentional in the NT structure definitions.
    target_compile_options(wrath_trace PRIVATE /W4 /WX /wd4201 /permissive- /EHsc /utf-8)
endif()

# ---- injection core (shared + inject objects, strict warnings) --------------
file(GLOB WRATH_INJECT_SRC
    "${CMAKE_SOURCE_DIR}/src/inject/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/inject/map/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/inject/run/*.cpp")
file(GLOB WRATH_SHARED_SRC
    "${CMAKE_SOURCE_DIR}/src/shared/**/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/shared/*.cpp")
add_library(wrath_inject STATIC ${WRATH_INJECT_SRC} ${WRATH_SHARED_SRC})
target_include_directories(wrath_inject PUBLIC ${WRATH_INCLUDE})
if(WRATH_SCREENSHARE_PRESET)
    target_compile_definitions(wrath_inject PUBLIC WRATH_SCREENSHARE_PRESET=1)
endif()
if(MSVC)
    # C4201 anonymous NT structs; C4100/C4189 unused params in the legacy mapper.
    target_compile_options(wrath_inject PRIVATE /W4 /WX /wd4201 /wd4100 /wd4189 /permissive- /EHsc /utf-8)
endif()

# ---- payload (client) --------------------------------------------------------
set(DT_WITH_INJECTOR OFF CACHE BOOL "" FORCE)
add_subdirectory(src/payload)
# The client target is display_service with OUTPUT_NAME display_service64.
add_library(WrathPayload ALIAS display_service)
add_library(dt_core ALIAS display_service)

# ---- loader ------------------------------------------------------------------
add_subdirectory(src/loader)

# ---- gate tool ---------------------------------------------------------------
file(GLOB WRATH_GATE_SRC "${CMAKE_SOURCE_DIR}/tools/gate/src/*.cpp")
add_executable(wrath_gate ${WRATH_GATE_SRC})
target_include_directories(wrath_gate PRIVATE ${WRATH_INCLUDE} ${CMAKE_SOURCE_DIR}/tools/gate/src)
target_link_libraries(wrath_gate PRIVATE wrath_trace wrath_inject)
if(MSVC)
    target_compile_options(wrath_gate PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
if(WRATH_SCREENSHARE_PRESET)
    add_custom_command(TARGET wrath_gate POST_BUILD
        COMMAND $<TARGET_FILE:wrath_gate> /check-auto
        COMMENT "Verifying x64-screenshare Auto strategy invariants")
endif()

# ---- probe tool (source created in the probe block) --------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/probe/main.cpp")
    add_executable(dt_diag "${CMAKE_SOURCE_DIR}/tools/probe/main.cpp")
    target_include_directories(dt_diag PRIVATE ${WRATH_INCLUDE})
    if(MSVC)
        target_compile_options(dt_diag PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    endif()
endif()

# ---- aggregate ---------------------------------------------------------------
add_custom_target(wrath ALL DEPENDS WrathLoader)
