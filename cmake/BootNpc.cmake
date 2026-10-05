include_guard(GLOBAL)
# This source-selected resource owner needs the real graphics inventory and
# frame-drain providers. It does not install NPC actors or a global manager.
if(TARGET charged_boot_effects)
    include(cmake/SkinPose.cmake)
    add_library(charged_boot_npc STATIC src/runtime/boot_npc.cpp)
    add_dependencies(charged_boot_npc verify_prepared)
    target_compile_features(charged_boot_npc PUBLIC cxx_std_20)
    target_link_libraries(charged_boot_npc PUBLIC charged_boot_effects
        charged_hierarchy_assets charged_sanim_assets charged_skin_pose)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        target_compile_options(charged_boot_npc PRIVATE -ffp-contract=off)
    endif()
    if(BUILD_TESTING)
        add_executable(boot_npc_tests tests/boot_npc.cpp)
        target_link_libraries(boot_npc_tests PRIVATE charged_boot_npc aurora::dvd aurora::core)
        add_test(NAME boot_npc COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_boot_npc.py" "$<TARGET_FILE:boot_npc_tests>")
        set_tests_properties(boot_npc PROPERTIES TIMEOUT 120)
    endif()
endif()
