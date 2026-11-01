# Mii heads drawn as the Wii draws its Mii icons (src/platform/mii_head.h),
# from the Mii parts of the player's game image: for the launcher, the game's
# overlay and a Mii editor. Plain C++, no SDL or ImGui.
include_guard(GLOBAL)
add_library(charged_mii_head STATIC src/platform/mii_head.cpp src/platform/mii_head_image.cpp)
target_include_directories(charged_mii_head PUBLIC src)
# The header is C++17 for the launcher's C++17 libraries; the drawing is C++20.
target_compile_features(charged_mii_head PUBLIC cxx_std_17 PRIVATE cxx_std_20)
target_link_libraries(charged_mii_head PRIVATE charged_host)

if(BUILD_TESTING)
    # Random Miis come from mii_maker.h; the test reads records with the
    # port's own record transport as well.
    include("${CMAKE_CURRENT_LIST_DIR}/MiiMaker.cmake")
    add_executable(mii_head_tests tests/mii_head.cpp)
    target_include_directories(mii_head_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(mii_head_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_options(mii_head_tests PRIVATE -fsigned-char -Wno-unknown-pragmas)
    target_link_libraries(mii_head_tests PRIVATE charged_mii_head charged_mii_maker)
    target_compile_features(mii_head_tests PRIVATE cxx_std_20)
    add_test(NAME mii_head COMMAND mii_head_tests)
    # MSCHARGED_TEST_DISC (a Charged image) adds checks with the real parts,
    # MSCHARGED_TEST_MII_REFERENCE a comparison with the game's own icons.
    set_tests_properties(mii_head PROPERTIES TIMEOUT 120)
endif()
