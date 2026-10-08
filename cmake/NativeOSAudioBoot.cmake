include_guard(GLOBAL)
include(cmake/NativeOSAudioRegisters.cmake)
include(cmake/NativeDSPBootMemory.cmake)
include(cmake/NativeAXBootstrap.cmake)
include(cmake/NativeAI.cmake)

add_library(charged_native_os_boot_environment STATIC src/platform/os_boot_environment.cpp)
add_dependencies(charged_native_os_boot_environment verify_prepared)
target_include_directories(charged_native_os_boot_environment PUBLIC src)
target_compile_features(charged_native_os_boot_environment PUBLIC cxx_std_17)
target_compile_definitions(charged_native_os_boot_environment PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_link_libraries(charged_native_os_boot_environment PUBLIC aurora::os)

add_library(charged_native_os_audio_boot STATIC src/platform/os_audio_boot_device.cpp)
add_dependencies(charged_native_os_audio_boot verify_prepared)
target_include_directories(charged_native_os_audio_boot PUBLIC src)
target_compile_features(charged_native_os_audio_boot PUBLIC cxx_std_17)
target_link_libraries(charged_native_os_audio_boot PUBLIC charged_native_dsp_boot_memory
    charged_native_os_boot_environment charged_native_ai charged_native_os_audio_registers)

# Complete original methods; explicit compiler inventory, not automatic OS/AX
# initialization. Authentic banks/cold register input and active kernel remain
# prerequisites. No source section collection or successful ready replacement.
add_library(charged_original_os_audio STATIC
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSAudioSystem.c")
add_dependencies(charged_original_os_audio verify_prepared)
target_include_directories(charged_original_os_audio PUBLIC src
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_os_audio PUBLIC MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_os_audio PRIVATE c_std_17)
target_link_libraries(charged_original_os_audio PUBLIC charged_native_os_audio_boot aurora::os)
set_target_properties(charged_original_os_audio PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    # Original OSTicks delta is signed32 wrap arithmetic on PPC/MWCC.
    target_compile_options(charged_original_os_audio PRIVATE -fexceptions -fwrapv
        -fno-strict-aliasing -Wno-unknown-pragmas)
endif()

# Bounded interop image: whole original OS init/stop plus original AX firmware
# bytes. Host owns the sole SDK; no linked runtime/GC or source stub in image.
add_library(native_os_audio_boot_source_fixture MODULE tests/os_audio_boot_source.c)
add_dependencies(native_os_audio_boot_source_fixture verify_prepared)
target_include_directories(native_os_audio_boot_source_fixture PRIVATE tests src
    "${MSCHARGED_PREPARED}" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(native_os_audio_boot_source_fixture PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_features(native_os_audio_boot_source_fixture PRIVATE c_std_17)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(native_os_audio_boot_source_fixture PRIVATE -fexceptions
        -fwrapv -fno-strict-aliasing -Wno-unknown-pragmas)
endif()

add_executable(native_os_audio_boot_tests tests/native_os_audio_boot.cpp
    src/platform/os.cpp src/platform/os_version.cpp)
add_dependencies(native_os_audio_boot_tests verify_prepared native_os_audio_boot_source_fixture)
target_include_directories(native_os_audio_boot_tests PRIVATE tests)
target_compile_features(native_os_audio_boot_tests PRIVATE cxx_std_17)
target_link_libraries(native_os_audio_boot_tests PRIVATE charged_original_dsp
    charged_native_os_audio_boot charged_native_ax_bootstrap SDL3::SDL3)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE)
    # Whole original dynamic image imports these genuine host providers.
    target_link_options(native_os_audio_boot_tests PRIVATE -Wl,--export-dynamic
        -Wl,--undefined=DCFlushRange -Wl,--undefined=OSGetTick
        -Wl,--undefined=OSGetArenaHi)
endif()
if(BUILD_TESTING)
    add_test(NAME native_os_audio_boot
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_dsp_backing.py"
            $<TARGET_FILE:native_os_audio_boot_tests> $<TARGET_FILE:native_os_audio_boot_source_fixture>)
    set_tests_properties(native_os_audio_boot PROPERTIES TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()

mscharged_add_native_os_audio_functional_stop_test()
