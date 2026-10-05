include_guard(GLOBAL)
# Complete reconstructed font units. The bounded CPU qualifier below exercises
# descriptor parsing/metrics; it does not establish DrawString or font readiness.
add_library(charged_original_fonts STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlFont.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTextEscape.cpp")
add_dependencies(charged_original_fonts verify_prepared)
target_compile_features(charged_original_fonts PUBLIC cxx_std_17)
target_include_directories(charged_original_fonts PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_original_fonts PUBLIC
    charged_original_core charged_game_print)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_fonts PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
elseif(MSVC)
    target_compile_options(charged_original_fonts PRIVATE /Gy /Gw)
endif()
if(BUILD_TESTING)
    add_executable(original_font_tests tests/original_font.cpp)
    target_compile_features(original_font_tests PRIVATE cxx_std_20)
    target_include_directories(original_font_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_font_tests PRIVATE
        charged_original_fonts charged_original_bundles charged_decomp_startup
        aurora::dvd aurora::core)
    # Drop unexecuted DrawString/GetExtendedColour only in this partial-link
    # qualifier. Their actual GL/string-support providers remain required by
    # the eventual whole-game production link.
    if(MSVC)
        target_compile_options(original_font_tests PRIVATE /Gy /Gw)
        target_link_options(original_font_tests PRIVATE /OPT:REF)
    elseif(APPLE)
        target_link_options(original_font_tests PRIVATE -Wl,-dead_strip)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(original_font_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME original_fonts COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_font.py"
        "$<TARGET_FILE:original_font_tests>")
    set_tests_properties(original_fonts PROPERTIES TIMEOUT 120
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()
