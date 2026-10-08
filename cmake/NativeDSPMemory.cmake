include_guard(GLOBAL)
include(cmake/NativeDSPMailbox.cmake)

# Physical address transport only for explicitly pinned live SDK MEM1 backing.
# Native module statics/MEM2 require a real shared physical ownership service;
# this target neither assigns guessed tokens nor executes/initializes a DSP.
add_library(charged_native_dsp_memory STATIC src/platform/dsp_memory.cpp)
add_dependencies(charged_native_dsp_memory verify_prepared)
target_include_directories(charged_native_dsp_memory PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_dsp_memory PUBLIC TARGET_PC=1)
target_compile_features(charged_native_dsp_memory PUBLIC cxx_std_17)
target_link_libraries(charged_native_dsp_memory PUBLIC charged_native_dsp_mailbox
    PRIVATE aurora::os)

# Bounded real source/SDK ELF qualifier. Original task transfers emit genuine
# mailbox words; the fixture only consumes/acks them. DSP boot/init/handler/MMIO
# sections remain collected, so this is not AX/movie/firmware readiness.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(native_dsp_memory_tests tests/native_dsp_memory.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_task.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp_debug.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c")
    add_dependencies(native_dsp_memory_tests verify_prepared)
    target_include_directories(native_dsp_memory_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_dsp_memory_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_dsp_memory_tests PRIVATE c_std_17 cxx_std_17)
    target_compile_options(native_dsp_memory_tests PRIVATE
        -ffunction-sections -fdata-sections -fexceptions -Wno-unknown-pragmas)
    target_link_libraries(native_dsp_memory_tests PRIVATE charged_native_dsp_memory aurora::os)
    target_link_options(native_dsp_memory_tests PRIVATE -Wl,--gc-sections)
    # Genuine OS memory/pin/mailbox qualifier without GX/window initialization.
    # It reuses this same test/provider graph in a GX-enabled Release build.
    add_test(NAME native_dsp_memory_read COMMAND native_dsp_memory_tests --memory-only)
    set_tests_properties(native_dsp_memory_read PROPERTIES TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    # This CPU prerequisite starts the real core SDK with SDL dummy video.
    # A GX-enabled SDK requires a real graphics surface before memory is reached;
    # that independent renderer lifecycle is outside this fixture.
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_dsp_memory COMMAND native_dsp_memory_tests)
        set_tests_properties(native_dsp_memory PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
endif()
