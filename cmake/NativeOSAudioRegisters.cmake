include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

# Sole borrowed MMIO dispatcher. Boot and functional devices bind the same
# original OS register ABI; neither links a second processor/mail/control owner.
add_library(charged_native_os_audio_registers STATIC src/platform/os_audio_registers.cpp)
add_dependencies(charged_native_os_audio_registers verify_prepared)
target_include_directories(charged_native_os_audio_registers PUBLIC src)
target_compile_features(charged_native_os_audio_registers PUBLIC cxx_std_17)
target_link_libraries(charged_native_os_audio_registers PUBLIC charged_native_interrupts)

# Called after either consumer is declared, so inclusion order creates this
# interop gate exactly once after both genuine source images are available.
function(mscharged_add_native_os_audio_functional_stop_test)
    if(NOT BUILD_TESTING OR TARGET native_os_audio_functional_stop_tests OR
       NOT TARGET charged_native_ax_functional OR
       NOT TARGET charged_native_os_boot_environment OR
       NOT TARGET native_ax_active_source_fixture OR
       NOT TARGET native_os_audio_boot_source_fixture)
        return()
    endif()
    include(cmake/OriginalModuleLinkage.cmake)
    add_executable(native_os_audio_functional_stop_tests
        tests/native_os_audio_functional_stop.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(native_os_audio_functional_stop_tests verify_prepared
        native_ax_active_source_fixture native_os_audio_boot_source_fixture)
    target_include_directories(native_os_audio_functional_stop_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_os_audio_functional_stop_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_os_audio_functional_stop_tests PRIVATE cxx_std_20)
    target_compile_options(native_os_audio_functional_stop_tests PRIVATE
        -fno-strict-aliasing -ffp-contract=off)
    target_link_libraries(native_os_audio_functional_stop_tests PRIVATE
        charged_native_ax_functional charged_native_os_boot_environment
        charged_native_thread_queues aurora::os SDL3::SDL3 ${CMAKE_DL_LIBS})
    get_target_property(ax_source_exports native_ax_init_tests LINK_OPTIONS)
    target_link_options(native_os_audio_functional_stop_tests PRIVATE ${ax_source_exports})
    foreach(symbol OSGetTick ChargedNativeOSInIPL ChargedOSAudioDSPRead
        ChargedOSAudioDSPWrite ChargedOSAudioDSPReadPair ChargedOSAudioDSPWritePair
        ChargedOSAudioIPCRead ChargedOSAudioIPCWrite ChargedOSAudioWorkMemory)
        mscharged_require_original_host_symbol(native_os_audio_functional_stop_tests PRIVATE "${symbol}")
    endforeach()
    add_test(NAME native_os_audio_functional_stop COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_os_audio_functional_stop.py"
        "$<TARGET_FILE:native_os_audio_functional_stop_tests>"
        "$<TARGET_FILE:native_ax_active_source_fixture>"
        "$<TARGET_FILE:native_os_audio_boot_source_fixture>")
    set_tests_properties(native_os_audio_functional_stop PROPERTIES TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" LABELS "Platform")
endfunction()
