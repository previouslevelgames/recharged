include_guard(GLOBAL)
# Explicitly bounded ELF link: the whole original TU is compiled, but this
# qualifier retains Init/Quit/unopened queries and the actual source mixer.
# Open/preload/async/NL/textures and original MovieInit(1)/AX are not qualified.
if(BUILD_TESTING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(original_movie_ai_tests tests/original_movie_ai.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/thp/THPSimple.cpp")
    add_dependencies(original_movie_ai_tests verify_prepared)
    target_include_directories(original_movie_ai_tests PRIVATE src
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(original_movie_ai_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 dSINGLE=1)
    target_compile_features(original_movie_ai_tests PRIVATE cxx_std_20)
    target_compile_options(original_movie_ai_tests PRIVATE
        -ffunction-sections -fdata-sections -ffp-contract=off
        -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
    target_link_options(original_movie_ai_tests PRIVATE -Wl,--gc-sections)
    target_link_libraries(original_movie_ai_tests PRIVATE
        charged_native_ai charged_thp_decoder aurora::os SDL3::SDL3)
    add_test(NAME original_movie_ai COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_ai.py"
        --original-only "$<TARGET_FILE:original_movie_ai_tests>")
    set_tests_properties(original_movie_ai PROPERTIES TIMEOUT 60)
endif()
