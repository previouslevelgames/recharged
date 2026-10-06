include_guard(GLOBAL)
# Whole original resources and pointer-width platform pool TUs. This target
# compiles the source graph; it does not suppress or satisfy remaining providers.
add_library(charged_original_texture_resources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/GL/GLInventory.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glResourcePool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxTexture.cpp")
add_dependencies(charged_original_texture_resources verify_prepared)
target_include_directories(charged_original_texture_resources PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_texture_resources PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_texture_resources PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_texture_resources PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
add_custom_target(charged_texture_resources_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_texture_resources>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-texture-resources-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_texture_resources
    COMMENT "Record whole original resource providers (not a runtime link check)"
    VERBATIM)
if(BUILD_TESTING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    # Explicit partial-link source qualifier. Uncalled loading/inventory/pool
    # vtable/rendering sections are discarded here, never from game production.
    add_executable(original_texture_animation_tests
        tests/original_texture_animation.cpp
        "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLInventory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManager.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp")
    target_compile_features(original_texture_animation_tests PRIVATE cxx_std_20)
    target_include_directories(original_texture_animation_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_options(original_texture_animation_tests PRIVATE
        -ffunction-sections -fdata-sections -ffp-contract=off
        -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
    if(APPLE)
        target_link_options(original_texture_animation_tests PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(original_texture_animation_tests PRIVATE -Wl,--gc-sections)
    endif()
    target_link_libraries(original_texture_animation_tests PRIVATE
        charged_original_core charged_game_print)
    add_test(NAME original_texture_animation COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_texture_animation.py"
        "$<TARGET_FILE:original_texture_animation_tests>"
        "${CMAKE_CURRENT_BINARY_DIR}/original-texture-animation-fixtures")
    set_tests_properties(original_texture_animation PROPERTIES TIMEOUT 90)
endif()
