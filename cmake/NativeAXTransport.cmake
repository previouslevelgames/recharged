include_guard(GLOBAL)
include(cmake/NativeDSPMemory.cmake)

# This ELF leaf is an explicit data-transport qualifier, not the original game
# module. All six source TUs are retained; AXOut/AXInit/DSP startup are absent.
# SDK services are imported from the sole SDK in the host, never linked here.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_library(native_ax_source_fixture MODULE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAlloc.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAux.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXCL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXComp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXSPB.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXVPB.c")
    add_dependencies(native_ax_source_fixture verify_prepared)
    target_include_directories(native_ax_source_fixture PRIVATE
        src "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_PREPARED}/libs/Runtime/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_ax_source_fixture PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_source_fixture PRIVATE c_std_17)
    target_compile_options(native_ax_source_fixture PRIVATE
        -fexceptions -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
    target_link_options(native_ax_source_fixture PRIVATE -Wl,--no-gc-sections)
    target_link_libraries(native_ax_source_fixture PRIVATE m)
    set_target_properties(native_ax_source_fixture PROPERTIES
        C_VISIBILITY_PRESET default POSITION_INDEPENDENT_CODE ON)

    add_executable(native_ax_transport_tests tests/native_ax_transport.cpp)
    add_dependencies(native_ax_transport_tests verify_prepared native_ax_source_fixture)
    target_include_directories(native_ax_transport_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_transport_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_transport_tests PRIVATE cxx_std_17)
    target_compile_options(native_ax_transport_tests PRIVATE -fno-strict-aliasing)
    target_link_libraries(native_ax_transport_tests PRIVATE
        charged_native_dsp_memory charged_native_interrupts aurora::os
        SDL3::SDL3 ${CMAKE_DL_LIBS})
    # Whole source C functions resolve these genuine cache/interrupt APIs at
    # dlopen. Force their actual implementations into the host export table.
    target_link_options(native_ax_transport_tests PRIVATE -Wl,--export-dynamic
        -Wl,--undefined=DCFlushRange -Wl,--undefined=DCFlushRangeNoSync
        -Wl,--undefined=DCInvalidateRange -Wl,--undefined=OSDisableInterrupts
        -Wl,--undefined=OSRestoreInterrupts)

    # A GX-enabled core needs a real surface before this memory-only gate runs.
    # Keep its binary compilable there; register execution only in the core SDK.
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_transport
            COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_transport.py"
            "$<TARGET_FILE:native_ax_transport_tests>"
            "$<TARGET_FILE:native_ax_source_fixture>")
        set_tests_properties(native_ax_transport PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
endif()
