cmake_minimum_required(VERSION 3.25)
project(mscharged_host_checks LANGUAGES C CXX)

# A small platform qualification build. This does not admit the game runtime
# on a new platform and never substitutes a launcher binary for the game.
include(CTest)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
get_filename_component(PORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MSCHARGED_PREPARED_BUILD_DIR "${PORT_ROOT}/build/release" CACHE PATH
    "Build directory containing verified prepared dependencies")
set(MSCHARGED_PREPARED_CLANG_FORMAT auto CACHE STRING
    "Formatter used to prepare the decomp: auto, off, or executable path")
get_filename_component(MSCHARGED_PREPARED_BUILD_DIR "${MSCHARGED_PREPARED_BUILD_DIR}" ABSOLUTE)
set(DECOMP "${MSCHARGED_PREPARED_BUILD_DIR}/prepared/mscharged-decomp/source")
set(AURORA "${MSCHARGED_PREPARED_BUILD_DIR}/prepared/aurora/source")
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR MSVC OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Host checks require a 64-bit GNU/Clang compiler (LLVM-MinGW on Windows)")
endif()

include("${PORT_ROOT}/cmake/NativeModuleLoader.cmake")
if(NOT EXISTS "${DECOMP}/src/RVL_SDK/os/OSMessage.c" OR
   NOT EXISTS "${AURORA}/include/dolphin/os.h")
    message(FATAL_ERROR "Prepare the pinned decomp and Aurora before configuring host checks")
endif()
add_custom_target(verify_host_sources
    COMMAND "${Python3_EXECUTABLE}" -B "${PORT_ROOT}/tools/prepare_sources.py"
        --build-dir "${MSCHARGED_PREPARED_BUILD_DIR}" --check
        --clang-format "${MSCHARGED_PREPARED_CLANG_FORMAT}"
    COMMAND "${Python3_EXECUTABLE}" -B "${PORT_ROOT}/tools/prepare_sources.py"
        --build-dir "${MSCHARGED_PREPARED_BUILD_DIR}" --dependency aurora --check
    VERBATIM)

include("${PORT_ROOT}/cmake/NativeListEntryHandles.cmake")
mscharged_add_list_entry_checks("${DECOMP}" verify_host_sources)
include("${PORT_ROOT}/cmake/NativeAudioTrackingKeys.cmake")
mscharged_add_audio_tracking_key_checks("${DECOMP}" "${AURORA}" verify_host_sources)
include("${PORT_ROOT}/cmake/NativeWordABI.cmake")
mscharged_add_native_word_checks("${DECOMP}" "${AURORA}" verify_host_sources)

function(mscharged_add_host_function_layout_check)
    set(MSCHARGED_PREPARED "${DECOMP}")
    set(MSCHARGED_AURORA_PREPARED "${AURORA}")
    include("${PORT_ROOT}/cmake/OriginalFunctionPools.cmake")
    if(TARGET original_function_layout_tests)
        add_dependencies(original_function_layout_tests verify_host_sources)
    endif()
endfunction()
mscharged_add_host_function_layout_check()

add_library(host_headers INTERFACE)
target_include_directories(host_headers INTERFACE "${PORT_ROOT}/src"
    "${AURORA}/include" "${DECOMP}/include" "${DECOMP}/libs/RVL_SDK/include")
target_compile_definitions(host_headers INTERFACE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(host_headers INTERFACE cxx_std_20)
target_link_libraries(host_headers INTERFACE Threads::Threads)
if(MINGW)
    # Keep fixture DLL/executable boundaries independent of redistributable CRT
    # DLL discovery; no game-owned allocator is introduced into the host CRT.
    target_link_options(host_headers INTERFACE -static)
endif()
add_library(host_interrupts STATIC "${PORT_ROOT}/src/platform/interrupts.cpp")
target_link_libraries(host_interrupts PUBLIC host_headers)
add_dependencies(host_interrupts verify_host_sources)
add_library(host_threads STATIC "${PORT_ROOT}/src/platform/thread_queues.cpp"
    "${PORT_ROOT}/src/platform/thread.cpp")
target_link_libraries(host_threads PUBLIC host_interrupts)
add_library(host_messages STATIC "${DECOMP}/src/RVL_SDK/os/OSMessage.c")
target_link_libraries(host_messages PUBLIC host_threads)
target_compile_features(host_messages PRIVATE c_std_17)
target_compile_options(host_messages PRIVATE -fexceptions -fno-strict-aliasing -Wno-unknown-pragmas)

function(host_check name source provider)
    add_executable("${name}" "${PORT_ROOT}/${source}" ${ARGN})
    target_link_libraries("${name}" PRIVATE "${provider}")
    add_test(NAME "${name}" COMMAND "${name}")
    set_tests_properties("${name}" PROPERTIES TIMEOUT 30)
endfunction()
host_check(native_ios tests/native_ios.cpp host_interrupts
    "${PORT_ROOT}/src/platform/ios_device.cpp")
add_executable(native_filesystem_physical
    "${PORT_ROOT}/tests/native_filesystem_physical.cpp"
    "${PORT_ROOT}/src/platform/filesystem_device.cpp"
    "${PORT_ROOT}/src/platform/ios_device.cpp")
target_link_libraries(native_filesystem_physical PRIVATE host_interrupts)
add_test(NAME native_filesystem_physical COMMAND native_filesystem_physical
    "${CMAKE_CURRENT_BINARY_DIR}/filesystem-test-data")
set_tests_properties(native_filesystem_physical PROPERTIES TIMEOUT 30)
host_check(native_thread_queues tests/native_thread_queues.cpp host_threads)
host_check(original_os_messages tests/original_os_messages.cpp host_messages)
host_check(native_thread_lifecycle tests/native_thread_lifecycle.cpp host_messages)

host_check(world_record_fields tests/world_record_fields.cpp host_headers)
add_dependencies(world_record_fields verify_host_sources)
target_compile_definitions(world_record_fields PRIVATE MSCHARGED_GAME_MODULE=1
    MSCHARGED_ORIGINAL_FUNCTION_POOLS=1 AURORA_WII_CLOCK=1 dSINGLE=1)
target_compile_options(world_record_fields PRIVATE -ffunction-sections
    -fdata-sections -fno-strict-aliasing -Wno-invalid-offsetof)
if(APPLE)
    target_link_options(world_record_fields PRIVATE LINKER:-dead_strip)
else()
    target_link_options(world_record_fields PRIVATE LINKER:--gc-sections)
endif()

add_executable(native_preferences_tests "${PORT_ROOT}/tests/native_preferences.cpp"
    "${PORT_ROOT}/src/resources/native_preferences.cpp"
    "${PORT_ROOT}/src/runtime/native_preferences.cpp")
target_link_libraries(native_preferences_tests PRIVATE host_headers)
add_dependencies(native_preferences_tests verify_host_sources)
if(NOT CMAKE_CROSSCOMPILING)
    # The Python oracle creates native filesystem paths. Cross-built fixtures
    # need a runner that maps those paths; native Windows CI runs this directly.
    add_test(NAME native_preferences COMMAND "${Python3_EXECUTABLE}" -B
        "${PORT_ROOT}/tests/test_native_preferences.py" "$<TARGET_FILE:native_preferences_tests>")
    set_tests_properties(native_preferences PROPERTIES TIMEOUT 45)
endif()
