include_guard(GLOBAL)

# Whole matched configuration/parser source under the original game-module ABI.
# The compiler inventory does not establish file callbacks, startup or CRT lifetime.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()

add_library(charged_original_config OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlConfig.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlDebug.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Sys/simpleparser.cpp")
add_dependencies(charged_original_config verify_prepared)
set_target_properties(charged_original_config PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_config PRIVATE cxx_std_20)
target_include_directories(charged_original_config PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_config PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_config PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_config PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_config PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_config PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
add_custom_target(charged_original_config_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_config>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-config-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_config
    COMMENT "Compile whole original configuration providers; startup/lifetime pending"
    VERBATIM)
