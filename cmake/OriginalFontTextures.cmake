include_guard(GLOBAL)
# Compile complete original font/texture units separately from old scene
# diagnostics. This compiler scan cannot establish callback/font readiness.
add_library(charged_original_font_textures OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Font/fontmanager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxTexture.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gc/gcSwizzler.cpp")
add_dependencies(charged_original_font_textures verify_prepared)
target_include_directories(charged_original_font_textures PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_font_textures PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_font_textures PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_font_textures PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
add_custom_target(charged_font_textures_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_font_textures>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-font-textures-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_font_textures
    COMMENT "Record original font/texture compiler providers (not a runtime link check)"
    VERBATIM)
if(BUILD_TESTING AND MSCHARGED_BUILD_GX_CHECK)
    add_executable(original_font_texture_abi_tests tests/original_font_texture_abi.cpp)
    target_compile_features(original_font_texture_abi_tests PRIVATE cxx_std_17)
    target_include_directories(original_font_texture_abi_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_font_texture_abi_tests PRIVATE
        charged_original_core aurora::gx)
    add_test(NAME original_font_texture_abi COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_font_texture_abi.py"
        "$<TARGET_FILE:original_font_texture_abi_tests>")
    set_tests_properties(original_font_texture_abi PROPERTIES TIMEOUT 90)
endif()
