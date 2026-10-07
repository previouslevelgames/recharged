include_guard(GLOBAL)
include(cmake/NativeThreadQueues.cmake)

# Complete original source compiler inventory. The object target deliberately
# exposes unresolved genuine DSP/OS providers rather than supplying substitutes.
add_library(charged_original_ax OBJECT
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AX.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAlloc.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAux.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXCL.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXComp.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXOut.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXProf.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXSPB.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXVPB.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mix/mix.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mix/remote.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXHooks.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXDelay.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXDelayExpDpl2.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXReverbHi.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXReverbHiDpl2.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXReverbHiExp.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/axfx/AXFXReverbHiExpDpl2.c")
add_dependencies(charged_original_ax verify_prepared)
target_include_directories(charged_original_ax PUBLIC
    src "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_ax PUBLIC
    MSCHARGED_NATIVE=1 TARGET_PC=1)
# Use the same canonical SDK policy as its native providers (including Wii clocks).
target_link_libraries(charged_original_ax PUBLIC aurora::os charged_native_thread_queues)
target_compile_features(charged_original_ax PRIVATE c_std_17)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_original_ax PRIVATE
        -ffunction-sections -fdata-sections -fexceptions -fno-strict-aliasing
        -Wno-unknown-pragmas)
endif()

# Explicitly bounded ELF CPU qualifier. All AX/MIX/AXFX units are compiled;
# section collection excludes original AXInit/DSP startup and hardware mixing.
# Voice descriptors below are fixture-owned rather than a fabricated AX init.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(original_ax_cpu_tests tests/original_ax_cpu.c)
    target_link_libraries(original_ax_cpu_tests PRIVATE
        charged_original_ax charged_native_interrupts aurora::os)
    target_compile_features(original_ax_cpu_tests PRIVATE c_std_17)
    target_compile_options(original_ax_cpu_tests PRIVATE
        -ffunction-sections -fdata-sections -fno-strict-aliasing
        -Wno-unknown-pragmas)
    target_link_options(original_ax_cpu_tests PRIVATE -Wl,--gc-sections)
    # The host interrupt implementation is C++; preserve its runtime provider.
    set_property(TARGET original_ax_cpu_tests PROPERTY LINKER_LANGUAGE CXX)
    add_test(NAME original_ax_cpu
        COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_ax_cpu.py"
        "$<TARGET_FILE:original_ax_cpu_tests>")
    set_tests_properties(original_ax_cpu PROPERTIES TIMEOUT 60)
endif()
