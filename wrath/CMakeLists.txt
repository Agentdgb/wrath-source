cmake_minimum_required(VERSION 3.24)

project(Wrath LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WRATH_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
set(WRATH_DEPLOY_DIR "${CMAKE_SOURCE_DIR}/../Injeccion" CACHE PATH
    "Runtime directory that receives WrathLoader, WrathPayload and their assets")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${WRATH_DIST_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${WRATH_DIST_DIR}")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${WRATH_DIST_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${WRATH_DIST_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

# WrathLoader is the only injector in the unified distribution.
set(VANTA_WITH_INJECTOR OFF CACHE BOOL "" FORCE)
option(WRATH_BUILD_TESTS "Build client regression tests" OFF)
option(VANTA_BUILD_TESTS "Build client regression tests (legacy alias)" OFF)
if(WRATH_BUILD_TESTS OR VANTA_BUILD_TESTS)
    set(VANTA_BUILD_TESTS ON CACHE BOOL "" FORCE)
    set(WRATH_BUILD_TESTS ON CACHE BOOL "" FORCE)
    enable_testing()
endif()

add_subdirectory(client)
add_subdirectory(loader)

add_custom_target(wrath ALL DEPENDS WrathLoader)
add_custom_target(WrathPayload DEPENDS display_service)
