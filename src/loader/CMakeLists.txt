set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui")

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(dependencies/freetype EXCLUDE_FROM_ALL)

add_executable(WrathLoader WIN32
    src/main.cpp
    src/auth/AuthClient.cpp
    src/injector/NtInjector.cpp
    src/version.rc
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(WrathLoader PRIVATE
    src
    ${CMAKE_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(WrathLoader PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_ENABLE_FREETYPE
)

if(MSVC)
    set_source_files_properties(
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        PROPERTIES COMPILE_FLAGS "/W0"
    )
    target_compile_options(WrathLoader PRIVATE /W3 /permissive- /utf-8 /bigobj /EHsc)
    target_link_options(WrathLoader PRIVATE $<$<CONFIG:Release>:/DEBUG:NONE> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    set_target_properties(WrathLoader PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\"")
endif()


target_link_libraries(WrathLoader PRIVATE
    wrath_trace
    freetype d3d11 d3dcompiler dxgi dwmapi windowscodecs ole32 comdlg32 shell32 advapi32 winhttp bcrypt
)



set_target_properties(WrathLoader PROPERTIES OUTPUT_NAME "WerFault")
add_dependencies(WrathLoader vantaclient)

add_custom_command(TARGET WrathLoader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WRATH_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WrathLoader>" "${WRATH_DEPLOY_DIR}/WerFault.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WrathLoader>" "${WRATH_DEPLOY_DIR}/DisplayTunerService.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:vantaclient>" "${WRATH_DEPLOY_DIR}/display_service64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WrathLoader>" "${WRATH_DEPLOY_DIR}/WrathLoader.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:vantaclient>" "${WRATH_DEPLOY_DIR}/WrathPayload.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${WRATH_DEPLOY_DIR}/assets"
    COMMENT "Deploying unified WerFault / DisplayTunerService + display_service64 runtime (embedded assets)"
    VERBATIM
)
