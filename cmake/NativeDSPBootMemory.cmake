include_guard(GLOBAL)
include(cmake/NativeDSPROM.cmake)

# Genuine boot DMA memory bytes only. GPIO/reset/ARAM cause/boot sequencing and
# authentic ROM are not provided by this scoped hardware transport.
add_library(charged_native_dsp_boot_memory STATIC src/platform/dsp_boot_memory.cpp)
add_dependencies(charged_native_dsp_boot_memory verify_prepared)
target_include_directories(charged_native_dsp_boot_memory PUBLIC src)
target_compile_features(charged_native_dsp_boot_memory PUBLIC cxx_std_17)
target_link_libraries(charged_native_dsp_boot_memory PUBLIC charged_native_dsp_instruction_core)

add_executable(native_dsp_boot_memory_tests tests/native_dsp_boot_memory.cpp
    src/platform/os_version.cpp src/platform/os.cpp)
add_dependencies(native_dsp_boot_memory_tests verify_prepared native_dsp_boot_data_fixture)
target_include_directories(native_dsp_boot_memory_tests PRIVATE tests)
target_compile_features(native_dsp_boot_memory_tests PRIVATE cxx_std_17)
target_link_libraries(native_dsp_boot_memory_tests PRIVATE charged_original_dsp
    charged_native_dsp_boot_memory SDL3::SDL3)
# The unchanged complete OS TU is the existing explicitly data-only source leaf.
# WholeDSP source host imports are retained; no source section collection here.
if(NOT MSCHARGED_BUILD_GX_CHECK)
    add_test(NAME native_dsp_boot_memory
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_dsp_backing.py"
            $<TARGET_FILE:native_dsp_boot_memory_tests> $<TARGET_FILE:native_dsp_boot_data_fixture>)
    set_tests_properties(native_dsp_boot_memory PROPERTIES TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
