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
set(WRATH_TEST_STAGING_DIR "" CACHE PATH "Optional staging directory for standalone JVM tests")
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

add_executable(wrath_storage_safety storage_safety.cpp
    ../src/core/config/ProfileStorage.cpp ../src/platform/ThreadUtil.cpp)
target_include_directories(wrath_storage_safety PRIVATE ../src ../vendor/json ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_storage_safety PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME wrath_storage_safety COMMAND wrath_storage_safety)
set_tests_properties(wrath_storage_safety PROPERTIES TIMEOUT 30)


add_executable(wrath_swordreplace_regression swordreplace_regression.cpp)
target_include_directories(wrath_swordreplace_regression PRIVATE ../src ${VANTA_JVM_INCLUDE_DIRS})
add_test(NAME wrath_swordreplace_regression COMMAND wrath_swordreplace_regression)
set_tests_properties(wrath_swordreplace_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_swordreplace_resources_regression swordreplace_resources_regression.cpp
    ../src/modules/render/swordreplace/SwordReplaceResources.cpp
    ../src/modules/render/swordreplace/SwordReplaceAssets.cpp)
add_dependencies(wrath_swordreplace_resources_regression packet_fixture)
target_include_directories(wrath_swordreplace_resources_regression PRIVATE ../src ../vendor/ankerl ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_swordreplace_resources_regression PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    WRATH_TEST_JVM_DLL="${VANTA_JAVA_HOME}/bin/server/jvm.dll"
    WRATH_TEST_CLASSPATH="${packet_fixture_dir}")
add_test(NAME wrath_swordreplace_resources_regression COMMAND wrath_swordreplace_resources_regression)
set_tests_properties(wrath_swordreplace_resources_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_visual_ports_regression visual_ports_regression.cpp
    ../src/modules/visuals/ports/VisualMath.cpp
    ../src/modules/visuals/ports/CharacterData.cpp
    ../assets/characters/characters.rc)
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
    ../src/modules/visuals/bloom/TargetEngine.cpp ../src/modules/visuals/bloom/PetalEngine.cpp)
target_include_directories(wrath_bloom_scene_regression PRIVATE ../src)
target_compile_definitions(wrath_bloom_scene_regression PRIVATE
    BLOOM_TEST_ANCHORS="${CMAKE_CURRENT_SOURCE_DIR}/../assets/bloom/original/petal-anchors.bin")
add_test(NAME wrath_bloom_scene_regression COMMAND wrath_bloom_scene_regression)
set_tests_properties(wrath_bloom_scene_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_bloom_death_regression bloom_death_regression.cpp
    ../src/modules/visuals/bloom/DeathEngine.cpp)
target_include_directories(wrath_bloom_death_regression PRIVATE ../src ${VANTA_JVM_INCLUDE_DIRS})
add_test(NAME wrath_bloom_death_regression COMMAND wrath_bloom_death_regression)
set_tests_properties(wrath_bloom_death_regression PROPERTIES TIMEOUT 30)

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

add_executable(wrath_bloom_atmosphere_regression bloom_atmosphere_regression.cpp
    ../src/modules/visuals/bloom/AtmosphereMath.cpp
    ../src/modules/visuals/bloom/native/SkyProfiles.cpp)
target_include_directories(wrath_bloom_atmosphere_regression PRIVATE ../src ${VANTA_JVM_INCLUDE_DIRS})
add_test(NAME wrath_bloom_atmosphere_regression COMMAND wrath_bloom_atmosphere_regression)
set_tests_properties(wrath_bloom_atmosphere_regression PROPERTIES TIMEOUT 30)

add_executable(wrath_bloom_gl_regression bloom_gl_regression.cpp
    ../src/modules/visuals/bloom/SkyRenderer.cpp
    ../src/modules/visuals/bloom/NativeTextures.cpp
    ../src/modules/visuals/bloom/AtmosphereMath.cpp
    ../src/modules/visuals/bloom/native/SkyGeometry.cpp
    ../src/modules/visuals/bloom/native/SkyProfiles.cpp
    ../src/modules/visuals/bloom/native/SpanArrayCache.cpp
    ../src/modules/visuals/bloom/native/StrataArrayCache.cpp
    ../src/modules/visuals/bloom/native/RibbonArrayCache.cpp
    ../src/modules/visuals/ports/CharacterData.cpp
    ../src/modules/render/arraylist/al_gl.cpp
    ../src/modules/render/arraylist/gl_ext.cpp
    ../assets/bloom/bloom.rc)
target_include_directories(wrath_bloom_gl_regression PRIVATE ../src ${VANTA_JVM_INCLUDE_DIRS})
target_compile_definitions(wrath_bloom_gl_regression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(wrath_bloom_gl_regression PRIVATE opengl32 user32 gdi32)
add_test(NAME wrath_bloom_gl_regression COMMAND wrath_bloom_gl_regression)
set_tests_properties(wrath_bloom_gl_regression PROPERTIES TIMEOUT 60)
