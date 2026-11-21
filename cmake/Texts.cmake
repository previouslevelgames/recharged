# Recharged's own texts in the player's language (src/bootstrap/texts.h): the
# launcher's and the in-game menus', from assets/lang (one folder per
# language, English complete).
include_guard(GLOBAL)
if(NOT TARGET charged_json)
    add_library(charged_json STATIC src/platform/json.cpp)
    target_include_directories(charged_json PUBLIC src)
    target_compile_features(charged_json PUBLIC cxx_std_17)
endif()
# English built in, generated from assets/lang/en (tools/embed_texts.py).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB MSCHARGED_ENGLISH_TEXTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang/en/*.json")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/texts/english_texts.cpp"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_texts.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang/en" "${CMAKE_CURRENT_BINARY_DIR}/generated/texts/english_texts.cpp"
    DEPENDS ${MSCHARGED_ENGLISH_TEXTS} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_texts.py"
    COMMENT "Building in Recharged's English texts"
    VERBATIM)
add_library(charged_texts STATIC src/bootstrap/texts.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/texts/english_texts.cpp")
target_include_directories(charged_texts PUBLIC src)
target_compile_features(charged_texts PUBLIC cxx_std_17)
target_link_libraries(charged_texts PUBLIC charged_json)
# The translations beside the program (assets/lang), copied again when one
# changes.
file(GLOB MSCHARGED_TEXT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang/*/*.json" "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang/README.md")
function(mscharged_stage_texts target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "$<TARGET_FILE_DIR:${target}>/assets/lang"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang"
                "$<TARGET_FILE_DIR:${target}>/assets/lang"
        VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${MSCHARGED_TEXT_FILES})
endfunction()
if(BUILD_TESTING)
    add_executable(texts_tests tests/texts.cpp)
    target_link_libraries(texts_tests PRIVATE charged_texts)
    add_test(NAME texts COMMAND texts_tests)
    # Every ID the code uses has its English text, and every translation's
    # texts match English's (IDs, placeholders, plural forms).
    add_test(NAME text_ids COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_texts.py"
        "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
