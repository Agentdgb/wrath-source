set(shieldbreaker_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/shieldbreaker-fixture")
add_custom_command(OUTPUT "${shieldbreaker_fixture_dir}/ShieldFixture.class"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${shieldbreaker_fixture_dir}"
    COMMAND "${VANTA_JAVA_HOME}/bin/javac.exe" -d "${shieldbreaker_fixture_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shieldbreaker-java/ShieldFixture.java"
    DEPENDS shieldbreaker-java/ShieldFixture.java VERBATIM)
add_custom_target(shieldbreaker_fixture DEPENDS "${shieldbreaker_fixture_dir}/ShieldFixture.class")
add_executable(wrath_shieldbreaker_regression shieldbreaker_regression.cpp
    ../src/modules/combat/shieldbreaker/ShieldBreaker.cpp)
add_dependencies(wrath_shieldbreaker_regression shieldbreaker_fixture)
target_include_directories(wrath_shieldbreaker_regression PRIVATE
    ../src ../vendor/vanta_compat ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_shieldbreaker_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${VANTA_JAVA_HOME}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${shieldbreaker_fixture_dir}")
add_test(NAME wrath_shieldbreaker_regression COMMAND wrath_shieldbreaker_regression)
set_tests_properties(wrath_shieldbreaker_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_regression
    regression.cpp
    ../src/core/event/EventBus.cpp
    ../src/core/config/ConfigManager.cpp
    ../src/core/config/ProfileStorage.cpp
    ../src/core/config/ShareCode.cpp
    ../src/core/social/FriendList.cpp
    ../src/util/Log.cpp
)
target_include_directories(wrath_regression PRIVATE
    ../src ../vendor ../vendor/json ../vendor/ankerl ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wrath_regression PRIVATE bcrypt)
add_test(NAME wrath_regression COMMAND wrath_regression)
set_tests_properties(wrath_regression PROPERTIES TIMEOUT 30)

set(throwpot_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/throwpot-fixture")
add_custom_command(OUTPUT "${throwpot_fixture_dir}/InputFixture.class"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${throwpot_fixture_dir}"
    COMMAND "${VANTA_JAVA_HOME}/bin/javac.exe" -d "${throwpot_fixture_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/throwpot-java/InputFixture.java"
    DEPENDS throwpot-java/InputFixture.java VERBATIM)
add_custom_target(throwpot_fixture DEPENDS "${throwpot_fixture_dir}/InputFixture.class")
add_executable(wrath_throwpot_input_regression throwpot_input_regression.cpp)
add_dependencies(wrath_throwpot_input_regression throwpot_fixture)
target_include_directories(wrath_throwpot_input_regression PRIVATE ../src ${VANTA_JVM_INCLUDE_DIRS})
file(TO_CMAKE_PATH "${VANTA_JAVA_HOME}" throwpot_java_home)
target_compile_definitions(wrath_throwpot_input_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${throwpot_java_home}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${throwpot_fixture_dir}")
add_test(NAME wrath_throwpot_input_regression COMMAND wrath_throwpot_input_regression)
set_tests_properties(wrath_throwpot_input_regression PROPERTIES TIMEOUT 30)

set(packet_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/packet-fixture")
file(GLOB_RECURSE packet_fixture_sources CONFIGURE_DEPENDS java/*.java)
add_custom_command(OUTPUT "${packet_fixture_dir}/net/minecraft/class_2535.class"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${packet_fixture_dir}"
    COMMAND "${VANTA_JAVA_HOME}/bin/javac.exe" -d "${packet_fixture_dir}" ${packet_fixture_sources}
    DEPENDS ${packet_fixture_sources} VERBATIM)
add_custom_target(packet_fixture DEPENDS "${packet_fixture_dir}/net/minecraft/class_2535.class")
add_executable(wrath_packet_regression packet_regression.cpp
    ../src/hook/PacketEvents.cpp ../src/core/event/EventBus.cpp ../src/util/Log.cpp
    ../src/jvm/JniHelper.cpp ../src/jvm/JniRef.cpp ../src/mappings/MappingRegistry.cpp)
file(GLOB packet_test_mappings CONFIGURE_DEPENDS ../src/mappings/generated/*.cpp)
target_sources(wrath_packet_regression PRIVATE ${packet_test_mappings})
add_dependencies(wrath_packet_regression packet_fixture)
target_include_directories(wrath_packet_regression PRIVATE
    ../src ../vendor ../vendor/ankerl ../vendor/jnihook/include ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_packet_regression PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${VANTA_JAVA_HOME}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${packet_fixture_dir}")
target_link_libraries(wrath_packet_regression PRIVATE vanta_jnihook)
if(NOT WRATH_TEST_STAGING_DIR OR WRATH_TEST_STAGING_DIR STREQUAL "" OR WRATH_TEST_STAGING_DIR MATCHES " ")
    set(WRATH_TEST_STAGING_DIR "C:/test_nospace" CACHE PATH "Optional staging directory for standalone JVM tests" FORCE)
endif()
if(WIN32 AND WRATH_TEST_STAGING_DIR)
    add_custom_command(TARGET wrath_packet_regression POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WRATH_TEST_STAGING_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:wrath_packet_regression>" "${WRATH_TEST_STAGING_DIR}/wrath_packet_regression.exe"
        VERBATIM)
    add_test(NAME wrath_packet_regression COMMAND "${WRATH_TEST_STAGING_DIR}/wrath_packet_regression.exe")
else()
    add_test(NAME wrath_packet_regression COMMAND wrath_packet_regression)
endif()
set_tests_properties(wrath_packet_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_jnihook_shutdown_regression jnihook_shutdown_regression.cpp
    ../vendor/jnihook/src/classfile.cpp ../vendor/jnihook/src/uuid.cpp
    ../vendor/jnihook/src/hook_tls_stub.cpp)
add_dependencies(wrath_jnihook_shutdown_regression packet_fixture)
target_include_directories(wrath_jnihook_shutdown_regression PRIVATE
    ../src ../vendor/jnihook/include ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_jnihook_shutdown_regression PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_JNIHOOK_FIXTURE="${packet_fixture_dir}/net/minecraft/class_2535.class")
add_test(NAME wrath_jnihook_shutdown_regression COMMAND wrath_jnihook_shutdown_regression)
set_tests_properties(wrath_jnihook_shutdown_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_diagnostics_regression diagnostics_regression.cpp)
target_include_directories(wrath_diagnostics_regression PRIVATE ../.. ../vendor/json)
target_compile_definitions(wrath_diagnostics_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wrath_diagnostics_regression PRIVATE shell32 ole32)
if(MSVC)
    target_compile_options(wrath_diagnostics_regression PRIVATE /GL- /Od)
endif()
add_test(NAME wrath_diagnostics_regression COMMAND wrath_diagnostics_regression)
set_tests_properties(wrath_diagnostics_regression PROPERTIES TIMEOUT 20)

add_executable(wrath_reaper_regression reaper_regression.cpp ../src/platform/ThreadUtil.cpp)
target_include_directories(wrath_reaper_regression PRIVATE ../src ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_reaper_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME wrath_reaper_regression COMMAND wrath_reaper_regression)
set_tests_properties(wrath_reaper_regression PROPERTIES TIMEOUT 15)

add_executable(wrath_storage_safety storage_safety.cpp
    ../src/core/config/ProfileStorage.cpp ../src/platform/ThreadUtil.cpp)
target_include_directories(wrath_storage_safety PRIVATE ../src ../vendor/json ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_storage_safety PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME wrath_storage_safety COMMAND wrath_storage_safety)
set_tests_properties(wrath_storage_safety PROPERTIES TIMEOUT 30)


file(GLOB_RECURSE spectate_fixture_sources CONFIGURE_DEPENDS spectate-java/*.java)
foreach(profile_api IN ITEMS record legacy)
    set(spectate_${profile_api}_dir "${CMAKE_CURRENT_BINARY_DIR}/spectate-${profile_api}")
    set(profile_source "${CMAKE_CURRENT_SOURCE_DIR}/spectate-${profile_api}/com/mojang/authlib/GameProfile.java")
    add_custom_command(OUTPUT "${spectate_${profile_api}_dir}/net/minecraft/SpectateFixture.class"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${spectate_${profile_api}_dir}"
        COMMAND "${VANTA_JAVA_HOME}/bin/javac.exe" -d "${spectate_${profile_api}_dir}" ${spectate_fixture_sources} "${profile_source}"
        DEPENDS ${spectate_fixture_sources} "${profile_source}" VERBATIM)
    add_custom_target(spectate_${profile_api}_fixture DEPENDS "${spectate_${profile_api}_dir}/net/minecraft/SpectateFixture.class")
endforeach()
add_executable(wrath_spectate_regression spectate_regression.cpp
    ../src/modules/misc/spectate/spectate.cpp ../src/util/Log.cpp)
add_dependencies(wrath_spectate_regression spectate_record_fixture spectate_legacy_fixture)
target_include_directories(wrath_spectate_regression PRIVATE ../src ../vendor/json ../vendor/ankerl ../vendor/jnihook/include ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_spectate_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${VANTA_JAVA_HOME}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${spectate_record_dir}" WRATH_TEST_LEGACY_CLASSPATH="${spectate_legacy_dir}")
target_link_libraries(wrath_spectate_regression PRIVATE vanta_jnihook)
add_test(NAME wrath_spectate_record_regression COMMAND wrath_spectate_regression record)
add_test(NAME wrath_spectate_legacy_regression COMMAND wrath_spectate_regression legacy)
set_tests_properties(wrath_spectate_record_regression wrath_spectate_legacy_regression PROPERTIES TIMEOUT 45)

add_executable(wrath_appearance_regression appearance_regression.cpp ../src/vgui/vanta/render/player_preview.cc)
target_include_directories(wrath_appearance_regression PRIVATE ../src ../vendor/json ../vendor/imgui)
target_link_libraries(wrath_appearance_regression PRIVATE vanta_imgui)
add_test(NAME wrath_appearance_regression COMMAND wrath_appearance_regression)
set_tests_properties(wrath_appearance_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_visual_ports_regression visual_ports_regression.cpp
    ../src/modules/visuals/ports/VisualMath.cpp)
target_include_directories(wrath_visual_ports_regression PRIVATE ../src ../vendor/json ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_visual_ports_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME wrath_visual_ports_regression COMMAND wrath_visual_ports_regression)
set_tests_properties(wrath_visual_ports_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_bloom_world_regression bloom_world_regression.cpp
    ../src/modules/visuals/bloom/WorldParticlesEngine.cpp)
target_include_directories(wrath_bloom_world_regression PRIVATE ../src)
add_test(NAME wrath_bloom_world_regression COMMAND wrath_bloom_world_regression)
set_tests_properties(wrath_bloom_world_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_bloom_scene_regression bloom_scene_regression.cpp
    ../src/modules/visuals/bloom/TargetEngine.cpp)
target_include_directories(wrath_bloom_scene_regression PRIVATE ../src)
add_test(NAME wrath_bloom_scene_regression COMMAND wrath_bloom_scene_regression)
set_tests_properties(wrath_bloom_scene_regression PROPERTIES TIMEOUT 30)

file(GLOB BLOOM_NATIVE_IMAGE_SOURCES ../src/modules/visuals/bloom/native/*ArrayCache.cpp)
add_executable(wrath_bloom_asset_baker bloom_asset_baker.cpp ${BLOOM_NATIVE_IMAGE_SOURCES})
target_include_directories(wrath_bloom_asset_baker PRIVATE ../src)
target_link_libraries(wrath_bloom_asset_baker PRIVATE windowscodecs ole32)
target_compile_definitions(wrath_bloom_asset_baker PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(wrath_bloom_sky_regression bloom_sky_regression.cpp
    ../src/modules/visuals/bloom/native/SkyGeometry.cpp
    ../src/modules/visuals/bloom/native/SkyProfiles.cpp
    ../src/modules/visuals/bloom/native/SpanArrayCache.cpp
    ../src/modules/visuals/bloom/native/StrataArrayCache.cpp
    ../src/modules/visuals/bloom/native/RibbonArrayCache.cpp)
target_include_directories(wrath_bloom_sky_regression PRIVATE ../src)
add_test(NAME wrath_bloom_sky_regression COMMAND wrath_bloom_sky_regression)
set_tests_properties(wrath_bloom_sky_regression PROPERTIES TIMEOUT 30)

set(visual_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/visual-fixture")
file(GLOB_RECURSE visual_fixture_sources CONFIGURE_DEPENDS visual-java/*.java)
add_custom_command(OUTPUT "${visual_fixture_dir}/net/minecraft/VisualFixture.class"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${visual_fixture_dir}"
    COMMAND "${VANTA_JAVA_HOME}/bin/javac.exe" -d "${visual_fixture_dir}" ${visual_fixture_sources}
    DEPENDS ${visual_fixture_sources} VERBATIM)
add_custom_target(visual_fixture DEPENDS "${visual_fixture_dir}/net/minecraft/VisualFixture.class")
add_executable(wrath_visual_runtime_regression visual_runtime_regression.cpp
    ../src/modules/visuals/ports/VisualHooks.cpp
    ../src/modules/visuals/ports/VisualMath.cpp
    ../src/modules/render/chams/item_shader_hook.cpp
    ../src/core/event/GameFramePump.cpp ../src/core/event/EventBus.cpp ../src/util/Log.cpp)
add_dependencies(wrath_visual_runtime_regression visual_fixture)
target_include_directories(wrath_visual_runtime_regression PRIVATE
    ../src ../vendor ../vendor/json ../vendor/ankerl ../vendor/jnihook/include ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_visual_runtime_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${VANTA_JAVA_HOME}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${visual_fixture_dir}")
target_link_libraries(wrath_visual_runtime_regression PRIVATE vanta_jnihook)
if(WIN32 AND WRATH_TEST_STAGING_DIR)
    add_custom_command(TARGET wrath_visual_runtime_regression POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WRATH_TEST_STAGING_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:wrath_visual_runtime_regression>" "${WRATH_TEST_STAGING_DIR}/wrath_visual_runtime_regression.exe"
        VERBATIM)
    add_test(NAME wrath_visual_runtime_regression COMMAND "${WRATH_TEST_STAGING_DIR}/wrath_visual_runtime_regression.exe")
else()
    add_test(NAME wrath_visual_runtime_regression COMMAND wrath_visual_runtime_regression)
endif()
set_tests_properties(wrath_visual_runtime_regression PROPERTIES TIMEOUT 60)

set(visibility_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/visibility-fixture")
file(GLOB_RECURSE visibility_fixture_sources CONFIGURE_DEPENDS visibility-java/*.java)
add_custom_command(OUTPUT "${visibility_fixture_dir}/net/minecraft/class_898.class"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${visibility_fixture_dir}"
    COMMAND "${VANTA_JAVA_HOME}/bin/javac.exe" -d "${visibility_fixture_dir}" ${visibility_fixture_sources}
    DEPENDS ${visibility_fixture_sources} VERBATIM)
add_custom_target(visibility_fixture DEPENDS "${visibility_fixture_dir}/net/minecraft/class_898.class")
add_executable(wrath_sodium_visibility_regression sodium_visibility_regression.cpp
    ../src/modules/render/chams/chams_cull.cpp)
add_dependencies(wrath_sodium_visibility_regression visibility_fixture)
target_include_directories(wrath_sodium_visibility_regression PRIVATE
    ../src ../vendor/json ../vendor/ankerl ../vendor/jnihook/include ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_sodium_visibility_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${VANTA_JAVA_HOME}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${visibility_fixture_dir}")
target_link_libraries(wrath_sodium_visibility_regression PRIVATE vanta_jnihook)
if(WIN32 AND WRATH_TEST_STAGING_DIR)
    add_custom_command(TARGET wrath_sodium_visibility_regression POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WRATH_TEST_STAGING_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:wrath_sodium_visibility_regression>" "${WRATH_TEST_STAGING_DIR}/wrath_sodium_visibility_regression.exe"
        VERBATIM)
    add_test(NAME wrath_sodium_visibility_regression COMMAND "${WRATH_TEST_STAGING_DIR}/wrath_sodium_visibility_regression.exe")
else()
    add_test(NAME wrath_sodium_visibility_regression COMMAND wrath_sodium_visibility_regression)
endif()
set_tests_properties(wrath_sodium_visibility_regression PROPERTIES TIMEOUT 45)

add_executable(wrath_actual_status_getter_probe actual_status_getter_probe.cpp)
target_include_directories(wrath_actual_status_getter_probe PRIVATE ../vendor/jnihook/include ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_actual_status_getter_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${VANTA_JAVA_HOME}/bin/server/jvm.dll")
target_link_libraries(wrath_actual_status_getter_probe PRIVATE vanta_jnihook)

add_executable(wrath_focus_visual_regression focus_visual_regression.cpp)
target_include_directories(wrath_focus_visual_regression PRIVATE ../src)
target_link_libraries(wrath_focus_visual_regression PRIVATE vanta_imgui)
add_test(NAME wrath_focus_visual_regression COMMAND wrath_focus_visual_regression)
set_tests_properties(wrath_focus_visual_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_aimassist_core_regression aimassist_core_regression.cpp)
target_include_directories(wrath_aimassist_core_regression PRIVATE ../src)
add_test(NAME wrath_aimassist_core_regression COMMAND wrath_aimassist_core_regression)
set_tests_properties(wrath_aimassist_core_regression PROPERTIES TIMEOUT 15)

file(TO_CMAKE_PATH "${VANTA_JAVA_HOME}" aimassist_java_home)
set(aimassist_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/aimassist-fixture")
add_custom_command(OUTPUT "${aimassist_fixture_dir}/net/minecraft/AimFixture.class"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${aimassist_fixture_dir}"
    COMMAND "${VANTA_JAVA_HOME}/bin/javac.exe" -d "${aimassist_fixture_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/aimassist-java/AimFixture.java"
        "${CMAKE_CURRENT_SOURCE_DIR}/aimassist-java/PortReference.java"
        "${CMAKE_CURRENT_SOURCE_DIR}/aimassist-java/reference/dev/readableaim/SimpleAimController.java"
        "${CMAKE_CURRENT_SOURCE_DIR}/aimassist-java/reference/dev/readableaim/WindAimController.java"
        "${CMAKE_CURRENT_SOURCE_DIR}/aimassist-java/reference/dev/readableaim/ManualAimOverride.java"
    DEPENDS aimassist-java/AimFixture.java aimassist-java/PortReference.java
        aimassist-java/reference/dev/readableaim/SimpleAimController.java
        aimassist-java/reference/dev/readableaim/WindAimController.java
        aimassist-java/reference/dev/readableaim/ManualAimOverride.java VERBATIM)
add_custom_target(aimassist_fixture DEPENDS "${aimassist_fixture_dir}/net/minecraft/AimFixture.class")
add_executable(wrath_aimassist_runtime_regression aimassist_runtime_regression.cpp
    ../src/modules/combat/aimassist/wind/aimassist_regular.cpp
    ../src/modules/combat/aimassist/wind/aimassist_cache.cpp
    ../src/modules/combat/aimassist/wind/WindSupport.cpp
    ../src/modules/combat/aimassist/wind/Engine.cpp)
add_dependencies(wrath_aimassist_runtime_regression aimassist_fixture)
target_include_directories(wrath_aimassist_runtime_regression PRIVATE
    ../src ../vendor ../vendor/json ../vendor/ankerl ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_aimassist_runtime_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${aimassist_java_home}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${aimassist_fixture_dir}")
add_test(NAME wrath_aimassist_runtime_regression COMMAND wrath_aimassist_runtime_regression)
set_tests_properties(wrath_aimassist_runtime_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_focus_range_regression focus_range_regression.cpp)
target_include_directories(wrath_focus_range_regression PRIVATE ../src)
add_test(NAME wrath_focus_range_regression COMMAND wrath_focus_range_regression)
set_tests_properties(wrath_focus_range_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_triggerbot_cooldown_regression triggerbot_cooldown_regression.cpp)
target_include_directories(wrath_triggerbot_cooldown_regression PRIVATE ../src)
add_test(NAME wrath_triggerbot_cooldown_regression COMMAND wrath_triggerbot_cooldown_regression)
set_tests_properties(wrath_triggerbot_cooldown_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_triggerbot_critical_regression triggerbot_critical_regression.cpp)
target_include_directories(wrath_triggerbot_critical_regression PRIVATE ../src)
add_test(NAME wrath_triggerbot_critical_regression COMMAND wrath_triggerbot_critical_regression)
set_tests_properties(wrath_triggerbot_critical_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_triggerbot_pending_click_regression triggerbot_pending_click_regression.cpp)
target_include_directories(wrath_triggerbot_pending_click_regression PRIVATE ../src)
add_test(NAME wrath_triggerbot_pending_click_regression COMMAND wrath_triggerbot_pending_click_regression)
set_tests_properties(wrath_triggerbot_pending_click_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_antibot_activity_regression antibot_activity_regression.cpp)
target_include_directories(wrath_antibot_activity_regression PRIVATE ../src)
add_test(NAME wrath_antibot_activity_regression COMMAND wrath_antibot_activity_regression)
set_tests_properties(wrath_antibot_activity_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_triggerbot_pipeline_regression triggerbot_pipeline_regression.cpp)
target_include_directories(wrath_triggerbot_pipeline_regression PRIVATE ../src)
add_test(NAME wrath_triggerbot_pipeline_regression COMMAND wrath_triggerbot_pipeline_regression)
set_tests_properties(wrath_triggerbot_pipeline_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_explosive_modules_regression explosive_modules_regression.cpp
    ../src/core/event/EventBus.cpp ../src/util/Log.cpp
    ../src/core/setting/Setting.cpp ../src/core/setting/BoolSetting.cpp
    ../src/core/setting/EnumSetting.cpp ../src/core/setting/NumericSetting.cpp
    ../src/core/setting/KeybindSetting.cpp)
target_include_directories(wrath_explosive_modules_regression PRIVATE
    ../src ../vendor ../vendor/json ../vendor/imgui ../vendor/vanta_compat ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_explosive_modules_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wrath_explosive_modules_regression PRIVATE vanta_imgui user32)
add_test(NAME wrath_explosive_modules_regression COMMAND wrath_explosive_modules_regression)
set_tests_properties(wrath_explosive_modules_regression PROPERTIES TIMEOUT 30)


add_executable(wrath_aimassist_port_parity aimassist_port_parity.cpp)
add_dependencies(wrath_aimassist_port_parity aimassist_fixture)
target_include_directories(wrath_aimassist_port_parity PRIVATE ../src ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_aimassist_port_parity PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${aimassist_java_home}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${aimassist_fixture_dir}")
if(MSVC)
 target_compile_options(wrath_aimassist_port_parity PRIVATE /fp:strict)
endif()
add_test(NAME wrath_aimassist_port_parity COMMAND wrath_aimassist_port_parity)
set_tests_properties(wrath_aimassist_port_parity PROPERTIES TIMEOUT 60)

if(MSVC)
 target_compile_options(wrath_aimassist_core_regression PRIVATE /fp:strict)
 target_compile_options(wrath_aimassist_runtime_regression PRIVATE /fp:strict)
endif()

add_executable(wrath_aimassist_ui_regression aimassist_ui_regression.cpp)
target_include_directories(wrath_aimassist_ui_regression PRIVATE ../src)
add_test(NAME wrath_aimassist_ui_regression COMMAND wrath_aimassist_ui_regression)
set_tests_properties(wrath_aimassist_ui_regression PROPERTIES TIMEOUT 15)
