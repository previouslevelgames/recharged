include_guard(GLOBAL)
# The whole original unit retains its actual static allocator constructors.
# Compilation does not establish production platform initialization before them.
add_library(charged_original_strings OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp")
add_dependencies(charged_original_strings verify_prepared)
target_compile_features(charged_original_strings PRIVATE cxx_std_17)
target_compile_definitions(charged_original_strings PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_include_directories(charged_original_strings PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
add_custom_target(charged_string_storage_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_strings>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-string-storage-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_strings
    COMMENT "Record original string providers (not a static startup link check)"
    VERBATIM)
if(BUILD_TESTING)
    add_executable(original_string_blocks_tests tests/original_string_blocks.cpp)
    target_compile_features(original_string_blocks_tests PRIVATE cxx_std_20)
    target_link_libraries(original_string_blocks_tests PRIVATE charged_original_core)
    add_test(NAME original_string_blocks COMMAND original_string_blocks_tests)
    set_tests_properties(original_string_blocks PROPERTIES TIMEOUT 30)
endif()
