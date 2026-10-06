include_guard(GLOBAL)
# The whole source is already in charged_original_task_flow. This independent
# ELF qualifier links only its unopened query sections and the genuine CPU
# codec. It does not link or initialize an AI/movie/render implementation.
if(BUILD_TESTING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(original_movie_data_tests
        tests/original_movie_data.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/thp/THPSimple.cpp")
    add_dependencies(original_movie_data_tests verify_prepared)
    target_include_directories(original_movie_data_tests PRIVATE src
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(original_movie_data_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 dSINGLE=1)
    target_compile_features(original_movie_data_tests PRIVATE cxx_std_20)
    target_compile_options(original_movie_data_tests PRIVATE
        -ffunction-sections -fdata-sections -ffp-contract=off
        -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
    target_link_options(original_movie_data_tests PRIVATE -Wl,--gc-sections)
    target_link_libraries(original_movie_data_tests PRIVATE charged_thp_decoder)
    add_test(NAME original_movie_data
        COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_movie_data.py"
        "$<TARGET_FILE:original_movie_data_tests>")
    set_tests_properties(original_movie_data PROPERTIES TIMEOUT 60)
endif()
