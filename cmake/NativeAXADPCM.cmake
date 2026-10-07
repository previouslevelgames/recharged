include_guard(GLOBAL)
include(cmake/NativeAXTransport.cmake)

# Raw DSP ADPCM arithmetic/descriptor transport only. Original four-tap SRC,
# complete voice mixing, DSP bootstrap and AX initialization remain unavailable.
add_library(charged_native_ax_adpcm STATIC src/platform/ax_adpcm_samples.cpp)
add_dependencies(charged_native_ax_adpcm verify_prepared)
target_include_directories(charged_native_ax_adpcm PUBLIC src PRIVATE
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_ax_adpcm PRIVATE MSCHARGED_NATIVE=1)
target_compile_features(charged_native_ax_adpcm PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_adpcm PUBLIC charged_native_dsp_memory)

if(BUILD_TESTING AND TARGET native_ax_source_fixture)
    # Retain all seven whole original producers in a single data-only leaf.
    # It imports the host's sole SDK; it does not link a second SDK instance.
    add_library(native_ax_adpcm_source_fixture MODULE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAlloc.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXAux.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXCL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXComp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXSPB.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/AXVPB.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/sp/sp.c")
    add_dependencies(native_ax_adpcm_source_fixture verify_prepared)
    target_include_directories(native_ax_adpcm_source_fixture PRIVATE
        src "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_PREPARED}/libs/Runtime/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_ax_adpcm_source_fixture PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_adpcm_source_fixture PRIVATE c_std_17)
    target_compile_options(native_ax_adpcm_source_fixture PRIVATE
        -fexceptions -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
    target_link_options(native_ax_adpcm_source_fixture PRIVATE -Wl,--no-gc-sections)
    target_link_libraries(native_ax_adpcm_source_fixture PRIVATE m)
    set_target_properties(native_ax_adpcm_source_fixture PROPERTIES
        C_VISIBILITY_PRESET default POSITION_INDEPENDENT_CODE ON)

    add_executable(native_ax_adpcm_tests tests/native_ax_adpcm.cpp)
    add_dependencies(native_ax_adpcm_tests verify_prepared native_ax_adpcm_source_fixture)
    target_include_directories(native_ax_adpcm_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_adpcm_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1)
    target_compile_features(native_ax_adpcm_tests PRIVATE cxx_std_17)
    target_compile_options(native_ax_adpcm_tests PRIVATE
        -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_adpcm_tests PRIVATE
        charged_native_ax_adpcm charged_native_interrupts aurora::core SDL3::SDL3)
    target_link_options(native_ax_adpcm_tests PRIVATE -Wl,--export-dynamic
        -Wl,--undefined=DCFlushRange -Wl,--undefined=DCFlushRangeNoSync
        -Wl,--undefined=DCInvalidateRange
        -Wl,--undefined=OSDisableInterrupts -Wl,--undefined=OSRestoreInterrupts)
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_adpcm
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_transport.py"
                $<TARGET_FILE:native_ax_adpcm_tests>
                $<TARGET_FILE:native_ax_adpcm_source_fixture>)
        set_tests_properties(native_ax_adpcm PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    endif()
endif()
