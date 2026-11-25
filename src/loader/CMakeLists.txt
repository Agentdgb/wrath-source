set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui")

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(dependencies/freetype EXCLUDE_FROM_ALL)

add_executable(WrathLoader WIN32
    main.cpp
    Paths.cpp
    daemon/ProcessWatcher.cpp
    auth/AuthClient.cpp
    ${CMAKE_SOURCE_DIR}/src/inject/NtInjector.cpp
    version.rc
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/display_tuner.manifest
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
    target_link_options(WrathLoader PRIVATE
        /DYNAMICBASE
        /HIGHENTROPYVA
        /NXCOMPAT
        /TSAWARE
        /MANIFEST:EMBED
        "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\""
        $<$<CONFIG:Release>:/DEBUG:NONE>
        $<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()


target_link_libraries(WrathLoader PRIVATE
    wrath_trace
    wrath_inject
    freetype d3d11 d3dcompiler dxgi dwmapi windowscodecs ole32 comdlg32 shell32 advapi32 winhttp bcrypt
)



set_target_properties(WrathLoader PROPERTIES OUTPUT_NAME "DisplayTuner")
add_dependencies(WrathLoader dt_core)

add_custom_command(TARGET WrathLoader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{ProgramData}/DisplayTuner"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WrathLoader>" "$ENV{ProgramData}/DisplayTuner/DisplayTuner.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:dt_core>" "$ENV{ProgramData}/DisplayTuner/display_core.dll"
    COMMAND icacls "$ENV{ProgramData}\\DisplayTuner" /grant "*S-1-5-32-545:(OI)(CI)M" /Q
    COMMENT "Deploying DisplayTuner.exe + display_core.dll to ProgramData/DisplayTuner"
    VERBATIM
)
