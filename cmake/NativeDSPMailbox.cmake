include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

# Genuine two-way DSP mailbox wire transport. Only an explicit connected device
# endpoint may exchange words or assert IRQ7. This does not execute firmware,
# install the original AX predecessor or synthesize boot/task-completion mail.
add_library(charged_native_dsp_mailbox STATIC src/platform/dsp_mailbox.cpp)
add_dependencies(charged_native_dsp_mailbox verify_prepared)
target_include_directories(charged_native_dsp_mailbox PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_dsp_mailbox PUBLIC TARGET_PC=1)
target_compile_features(charged_native_dsp_mailbox PUBLIC cxx_std_17)
target_link_libraries(charged_native_dsp_mailbox PUBLIC charged_native_interrupt_controller)

# Bounded ELF CPU hardware qualifier. The whole original DSP TU is compiled;
# collection retains the four genuine mailbox functions and DSPCheckInit, while
# excluding DSPInit/AssertInt/AddTask/AssertTask and their missing engine/MMIO
# providers. The device backend below is explicitly a wire-injection fixture.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(native_dsp_mailbox_tests tests/native_dsp_mailbox.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c")
    add_dependencies(native_dsp_mailbox_tests verify_prepared)
    target_include_directories(native_dsp_mailbox_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_dsp_mailbox_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_dsp_mailbox_tests PRIVATE c_std_17 cxx_std_17)
    target_compile_options(native_dsp_mailbox_tests PRIVATE
        -ffunction-sections -fdata-sections -fexceptions)
    target_link_libraries(native_dsp_mailbox_tests PRIVATE charged_native_dsp_mailbox aurora::os)
    target_link_options(native_dsp_mailbox_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME native_dsp_mailbox COMMAND native_dsp_mailbox_tests)
    set_tests_properties(native_dsp_mailbox PROPERTIES TIMEOUT 30)
endif()
