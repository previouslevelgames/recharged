include_guard(GLOBAL)

# Whole original AXSetVoiceAddr and PrintStudio methods with native packed-word
# access. This is a bounded ELF data qualifier; section collection excludes
# original AXInit/Out/DSP startup, not a substitute for those genuine services.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(original_ax_words_tests tests/original_ax_words.c)
    target_link_libraries(original_ax_words_tests PRIVATE
        charged_original_ax charged_native_interrupts aurora::os)
    target_compile_features(original_ax_words_tests PRIVATE c_std_17)
    target_compile_options(original_ax_words_tests PRIVATE
        -ffunction-sections -fdata-sections -fno-strict-aliasing
        -Wno-unknown-pragmas)
    target_link_options(original_ax_words_tests PRIVATE -Wl,--gc-sections)
    set_property(TARGET original_ax_words_tests PROPERTY LINKER_LANGUAGE CXX)
    add_test(NAME original_ax_words
        COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_ax_words.py"
        "$<TARGET_FILE:original_ax_words_tests>")
    set_tests_properties(original_ax_words PROPERTIES TIMEOUT 60)
endif()
