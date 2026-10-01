cmake_minimum_required(VERSION 3.25)
project(mscharged_port VERSION 0.0.1 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build -G Ninja")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Git REQUIRED)
include(CTest)

set(MSCHARGED_PREPARER "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_sources.py")
set(MSCHARGED_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/mscharged-decomp")
file(GLOB patch_inputs CONFIGURE_DEPENDS "${MSCHARGED_PATCHES}/*")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path index
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE git_index OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
get_filename_component(git_index "${git_index}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MSCHARGED_PREPARER}" "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules"
    "${git_index}" ${patch_inputs})

# Preparation precedes all source selection and configuration. It never fetches
# an upstream branch or edits the submodule working tree.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${MSCHARGED_PREPARER}"
            --build-dir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE MSCHARGED_PREPARED OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
file(TO_CMAKE_PATH "${MSCHARGED_PREPARED}" MSCHARGED_PREPARED)
file(READ "${MSCHARGED_PATCHES}/base" MSCHARGED_DECOMP_REVISION)
string(STRIP "${MSCHARGED_DECOMP_REVISION}" MSCHARGED_DECOMP_REVISION)

add_custom_target(verify_prepared ALL
    COMMAND "${Python3_EXECUTABLE}" -B "${MSCHARGED_PREPARER}"
            --build-dir "${CMAKE_BINARY_DIR}" --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Verify pinned, unmodified upstream and prepared sources"
    VERBATIM
)

# This list deliberately covers only currently available, independently linked
# source. The incomplete decomp is not yet a complete native game target.
add_library(charged_foundation STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlEndian.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlRandom.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Core/mtRandom.cpp"
)
add_dependencies(charged_foundation verify_prepared)
target_include_directories(charged_foundation PUBLIC "${MSCHARGED_PREPARED}/include")
target_compile_features(charged_foundation PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_foundation PUBLIC -ffp-contract=off -fno-strict-aliasing)
    target_compile_options(charged_foundation PRIVATE -Wno-unknown-pragmas)
endif()

add_executable(mscharged-bootstrap src/bootstrap/main.cpp)
target_link_libraries(mscharged-bootstrap PRIVATE charged_foundation)
target_compile_definitions(mscharged-bootstrap PRIVATE
    MSCHARGED_DECOMP_REVISION="${MSCHARGED_DECOMP_REVISION}")

if(BUILD_TESTING)
    add_executable(foundation_tests tests/foundation.cpp)
    target_link_libraries(foundation_tests PRIVATE charged_foundation)
    add_test(NAME foundation COMMAND foundation_tests)
    add_test(NAME source_preparation
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_sources.py")
endif()

message(STATUS "Configured the development bootstrap; the complete game, Aurora runtime, and UI are not built yet")
