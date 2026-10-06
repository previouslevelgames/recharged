include_guard(GLOBAL)
# Original source owns language/file selection, lookup and loading state. Native
# transport keeps the Wii20/8-byte serialized geometry in the same NL allocation.
add_library(charged_original_localization STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlLocalization.cpp"
    src/platform/localization_data.cpp)
add_dependencies(charged_original_localization verify_prepared)
target_compile_features(charged_original_localization PUBLIC cxx_std_20)
target_include_directories(charged_original_localization PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_original_localization PUBLIC
    charged_original_core charged_game_print)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_localization PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
elseif(MSVC)
    target_compile_options(charged_original_localization PRIVATE /Gy /Gw)
endif()

# Compile the entire genuine caller; its main/GameInfo/font loading providers
# remain required by the game graph. This scan does not fabricate font readiness.
add_library(charged_original_font_loading OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Font/FontLoading.cpp")
add_dependencies(charged_original_font_loading verify_prepared)
target_include_directories(charged_original_font_loading PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_original_font_loading PRIVATE charged_original_localization)
add_custom_target(charged_font_loading_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_font_loading>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-font-loading-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_font_loading
    COMMENT "Record whole original font loading providers (not font readiness)"
    VERBATIM)

if(BUILD_TESTING)
    add_executable(original_localization_tests tests/original_localization.cpp)
    target_compile_features(original_localization_tests PRIVATE cxx_std_20)
    target_include_directories(original_localization_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_localization_tests PRIVATE
        charged_original_localization aurora::core aurora::dvd SDL3::SDL3)
    if(MSVC)
        target_compile_options(original_localization_tests PRIVATE /Gy /Gw)
        target_link_options(original_localization_tests PRIVATE /OPT:REF)
    elseif(APPLE)
        target_link_options(original_localization_tests PRIVATE -Wl,-dead_strip)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(original_localization_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME original_localization COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_localization.py"
        "$<TARGET_FILE:original_localization_tests>")
    set_tests_properties(original_localization PROPERTIES TIMEOUT 90
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()
