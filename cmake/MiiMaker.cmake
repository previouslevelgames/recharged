include_guard(GLOBAL)
# New Miis for the launcher (src/platform/mii_maker.h): the Wii's own random
# face, RFL_MakeRandomFace.c, built for the host. It draws from OSGetTick;
# here that is ChargedMiiRandomTick, a seeded engine. The Mii library's
# headers need Aurora's dolphin/gx.h.
if(NOT MSCHARGED_AURORA_PREPARED)
    mscharged_prepare_dependency(aurora MSCHARGED_AURORA_PREPARED)
endif()
add_library(charged_mii_maker STATIC
    "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_MakeRandomFace.c"
    src/platform/rfl_character_transport.c
    src/platform/rfl_random_mii.c
    src/platform/mii_maker.cpp)
add_dependencies(charged_mii_maker verify_prepared)
target_include_directories(charged_mii_maker PRIVATE
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_include_directories(charged_mii_maker PUBLIC src)
target_compile_features(charged_mii_maker PRIVATE c_std_17 cxx_std_17)
# On the target, not the source: the online game module compiles the same
# RFL_MakeRandomFace.c with the Wii's OSGetTick.
target_compile_definitions(charged_mii_maker PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1
    OSGetTick=ChargedMiiRandomTick)
target_compile_options(charged_mii_maker PRIVATE -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
set_target_properties(charged_mii_maker PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)

if(BUILD_TESTING)
    add_executable(mii_maker_tests tests/mii_maker.cpp)
    target_compile_features(mii_maker_tests PRIVATE cxx_std_17)
    target_include_directories(mii_maker_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(mii_maker_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_options(mii_maker_tests PRIVATE -fsigned-char -Wno-unknown-pragmas)
    target_link_libraries(mii_maker_tests PRIVATE charged_mii_maker)
    add_test(NAME mii_maker COMMAND mii_maker_tests)
    set_tests_properties(mii_maker PROPERTIES TIMEOUT 30)
endif()
