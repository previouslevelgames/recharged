include_guard(GLOBAL)
include(cmake/NativeAXActiveVoice.cmake)

# Optional explicit platform coefficient policy. This does not attach a device,
# populate ISA/DROM state, select a source voice or authorize audio startup.
# Strict supplied-bank/normal-device entry points retain their original contract.
add_library(charged_native_ax_native_filter STATIC src/platform/ax_native_filter.cpp)
add_dependencies(charged_native_ax_native_filter verify_prepared)
target_include_directories(charged_native_ax_native_filter PUBLIC src)
target_compile_features(charged_native_ax_native_filter PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_native_filter PUBLIC charged_native_ax_active_voice)

if(BUILD_TESTING AND TARGET native_ax_active_source_fixture)
    set(native_filter_oracles "${CMAKE_CURRENT_BINARY_DIR}/native-ax-filter-oracles")
    add_custom_command(OUTPUT "${native_filter_oracles}/oracle.bin"
            "${native_filter_oracles}/mix-oracle.bin" "${native_filter_oracles}/native-filter-rows.bin"
            "${native_filter_oracles}/native-continuation.bin"
            "${native_filter_oracles}/lpf-oracle.bin" "${native_filter_oracles}/scope.json"
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_filter_oracle.py"
            "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c" "${native_filter_oracles}"
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_lpf_oracle.py"
            "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
            "${native_filter_oracles}/mix-oracle.bin" "${native_filter_oracles}/lpf-oracle.bin"
        DEPENDS verify_prepared "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/DSPCode.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_filter_oracle.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_voice_oracle.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_ax_lpf_oracle.py" VERBATIM)
    add_custom_target(native_ax_filter_oracles DEPENDS "${native_filter_oracles}/oracle.bin"
        "${native_filter_oracles}/mix-oracle.bin" "${native_filter_oracles}/native-filter-rows.bin"
        "${native_filter_oracles}/native-continuation.bin" "${native_filter_oracles}/lpf-oracle.bin"
        "${native_filter_oracles}/scope.json")
    add_executable(native_ax_native_filter_tests tests/native_ax_native_filter.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_ax_native_filter_tests verify_prepared
        native_ax_active_source_fixture native_ax_filter_oracles)
    target_include_directories(native_ax_native_filter_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ax_native_filter_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_ax_native_filter_tests PRIVATE cxx_std_20)
    target_compile_options(native_ax_native_filter_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_ax_native_filter_tests PRIVATE charged_native_ax_native_filter
        charged_native_ax_bootstrap charged_native_thread_queues charged_native_ai
        aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    get_target_property(ax_source_exports native_ax_init_tests LINK_OPTIONS)
    target_link_options(native_ax_native_filter_tests PRIVATE ${ax_source_exports})
    if(NOT MSCHARGED_BUILD_GX_CHECK)
        add_test(NAME native_ax_native_filter COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_native_filter.py"
            "$<TARGET_FILE:native_ax_native_filter_tests>"
            "$<TARGET_FILE:native_ax_active_source_fixture>" "${native_filter_oracles}")
        set_tests_properties(native_ax_native_filter PROPERTIES TIMEOUT 45
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
    endif()
endif()
