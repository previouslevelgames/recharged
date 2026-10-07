include_guard(GLOBAL)
include(cmake/NativeDSPMemory.cmake)
include(cmake/NativeAI.cmake)
include(cmake/OriginalFunctionPools.cmake)

# Explicit ELF hardware/ownership fixture. Its platform-only early reservation
# runs before the real eager source pools; this is not the production loader or
# a qualification of AXInit, DSP firmware, mode1 movie audio or full-game CRT.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT MSVC
        AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_library(native_ax_output_source_fixture MODULE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAlloc.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAux.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXCL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXComp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXSPB.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXVPB.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXProf.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXOut.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dsp/dsp.c"
        "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
        "${MSCHARGED_PREPARED}/src/Game/DetermDataEvent.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlFunctionMemory.cpp"
        src/platform/game_allocation_ownership.cpp
        src/platform/game_module_allocations.cpp
        tests/fixtures/native_ax_output_source.cpp)
    add_dependencies(native_ax_output_source_fixture verify_prepared)
    target_include_directories(native_ax_output_source_fixture PRIVATE
        tests "${MSCHARGED_AURORA_PREPARED}/include")
    # Header/profile usage only: no SDK/CRT provider is linked into this image.
    target_link_libraries(native_ax_output_source_fixture PRIVATE
        charged_original_function_pool_abi m)
    target_compile_definitions(native_ax_output_source_fixture PRIVATE
        MSCHARGED_GAME_MODULE=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_output_source_fixture PRIVATE c_std_17 cxx_std_20)
    target_compile_options(native_ax_output_source_fixture PRIVATE
        -fexceptions -ffunction-sections -fdata-sections -fno-strict-aliasing
        -ffp-contract=off -fsigned-char -Wno-unknown-pragmas
        "$<$<COMPILE_LANGUAGE:CXX>:-fcheck-new>")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(native_ax_output_source_fixture PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique;-fno-assume-sane-operators-new-delete>")
    else()
        target_compile_options(native_ax_output_source_fixture PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fno-assume-sane-operator-new;-Wno-register>")
    endif()
    set_target_properties(native_ax_output_source_fixture PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    # Unrelated AX/DSP initialization/task/handler functions are collected.
    # The exact retained CPU methods and imports are recorded by the gate.
    target_link_options(native_ax_output_source_fixture PRIVATE
        -Wl,-Bsymbolic-functions -Wl,--gc-sections
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native_ax_output_exports.map")

    add_executable(native_ax_output_tests tests/native_ax_output.cpp src/platform/os.cpp)
    add_dependencies(native_ax_output_tests verify_prepared native_ax_output_source_fixture)
    target_include_directories(native_ax_output_tests PRIVATE tests
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_output_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_output_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_output_tests PRIVATE -fno-strict-aliasing)
    target_link_libraries(native_ax_output_tests PRIVATE charged_native_dsp_memory
        charged_native_ai charged_native_metadata aurora::os aurora::dvd
        SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_link_options(native_ax_output_tests PRIVATE -Wl,--export-dynamic
        -Wl,--undefined=DCFlushRange -Wl,--undefined=DCFlushRangeNoSync
        -Wl,--undefined=DCInvalidateRange -Wl,--undefined=OSDisableInterrupts
        -Wl,--undefined=OSRestoreInterrupts -Wl,--undefined=DVDInit
        -Wl,--undefined=VIInit -Wl,--undefined=OSGetMEM2ArenaLo
        -Wl,--undefined=OSGetMEM2ArenaHi -Wl,--undefined=OSAllocFromMEM2ArenaLo
        -Wl,--undefined=OSInitAlloc -Wl,--undefined=OSCreateHeap
        -Wl,--undefined=OSSetCurrentHeap -Wl,--undefined=OSReport
        -Wl,--undefined=OSGetConsoleSimulatedMem2Size -Wl,--undefined=OSGetTime
        -Wl,--undefined=ChargedNativeMetadataAllocate -Wl,--undefined=ChargedNativeMetadataRelease)
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_output COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_transport.py"
            "$<TARGET_FILE:native_ax_output_tests>"
            "$<TARGET_FILE:native_ax_output_source_fixture>")
        set_tests_properties(native_ax_output PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
