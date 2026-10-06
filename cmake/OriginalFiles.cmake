include_guard(GLOBAL)

# Complete original NL raw/whole-file TUs, with the original module ABI. This
# compiler graph must be composed with the module's one source allocator/pool
# graph and the host's one SDK foundation. It does not establish main startup.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_file_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlFileGC.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFile.cpp"
    src/platform/file_handle_abi.cpp)
add_dependencies(charged_original_file_module_sources verify_prepared)
include(cmake/OriginalCompressedFiles.cmake)
mscharged_add_original_inflater(charged_original_file_module_sources)
set_target_properties(charged_original_file_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_file_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_file_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_file_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os aurora::dvd)
target_compile_definitions(charged_original_file_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_file_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_file_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_file_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()

if(BUILD_TESTING)
    # Synthetic pointer identities exercise the ABI substrate only. The test
    # supplies a failing metadata allocator; no game/file providers are mocked.
    add_executable(file_handle_abi_tests
        tests/file_handle_abi.cpp src/platform/file_handle_abi.cpp)
    target_compile_features(file_handle_abi_tests PRIVATE cxx_std_20)
    target_include_directories(file_handle_abi_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME file_handle_abi COMMAND file_handle_abi_tests)
endif()
