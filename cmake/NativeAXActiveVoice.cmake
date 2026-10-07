include_guard(GLOBAL)
include(cmake/NativeAXInitialization.cmake)
include(cmake/NativeAXADPCM.cmake)

# Hardware parameter-block arithmetic only. The caller must supply an authentic
# coefficient DROM before production use; tests use a deliberate synthetic bank.
# This library is not attached as a command kernel or a ready active device.
add_library(charged_native_ax_active_voice STATIC src/platform/ax_active_voice.cpp)
add_dependencies(charged_native_ax_active_voice verify_prepared)
target_include_directories(charged_native_ax_active_voice PUBLIC src)
target_compile_features(charged_native_ax_active_voice PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_active_voice PUBLIC charged_native_ax_adpcm)

if(BUILD_TESTING AND TARGET native_ax_init_source_fixture)
    # The same whole13 initialization TUs plus complete original SP/MIX/remote.
    # No method extraction, section collection or second SDK inside this image.
    get_target_property(ax_original_sources native_ax_init_source_fixture SOURCES)
    add_library(native_ax_active_source_fixture MODULE ${ax_original_sources}
        "${MSCHARGED_PREPARED}/src/RVL_SDK/sp/sp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mix/mix.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mix/remote.c")
    add_dependencies(native_ax_active_source_fixture verify_prepared)
    target_include_directories(native_ax_active_source_fixture PRIVATE src
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_PREPARED}/libs/Runtime/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_ax_active_source_fixture PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(native_ax_active_source_fixture PRIVATE c_std_17)
    target_compile_options(native_ax_active_source_fixture PRIVATE
        -fexceptions -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
    set_target_properties(native_ax_active_source_fixture PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default)
    target_link_libraries(native_ax_active_source_fixture PRIVATE m)
    target_link_options(native_ax_active_source_fixture PRIVATE -Wl,-Bsymbolic-functions -Wl,--no-gc-sections)

    set(ax_voice_oracles "${CMAKE_CURRENT_BINARY_DIR}/native-ax-voice-oracles")
    add_custom_command(OUTPUT "${ax_voice_oracles}/oracle.bin"
            "${ax_voice_oracles}/mix-oracle.bin" "${ax_voice_oracles}/synthetic-drom.bin"
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_voice_oracle.py"
            "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c" "${ax_voice_oracles}"
        DEPENDS verify_prepared "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_voice_oracle.py" VERBATIM)
    add_custom_target(native_ax_voice_oracles DEPENDS "${ax_voice_oracles}/oracle.bin"
        "${ax_voice_oracles}/mix-oracle.bin" "${ax_voice_oracles}/synthetic-drom.bin")
    add_executable(native_ax_active_tests tests/native_ax_active.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_active_tests verify_prepared native_ax_active_source_fixture native_ax_voice_oracles)
    target_include_directories(native_ax_active_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_active_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_active_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_active_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_active_tests PRIVATE charged_native_ax_active_voice
        charged_native_ax_bootstrap charged_native_thread_queues charged_native_ai
        aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    # Exact retained initialization fixture imports; this host owns the sole SDK.
    get_target_property(ax_source_exports native_ax_init_tests LINK_OPTIONS)
    target_link_options(native_ax_active_tests PRIVATE ${ax_source_exports})
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_active COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_active.py"
            "$<TARGET_FILE:native_ax_active_tests>" "$<TARGET_FILE:native_ax_active_source_fixture>"
            "${ax_voice_oracles}")
        set_tests_properties(native_ax_active PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
