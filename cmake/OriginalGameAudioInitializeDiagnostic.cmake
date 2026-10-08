include_guard(GLOBAL)

# Separate from constructor-only404. Normal startup, Credits and owner-only
# frontend modules retain their existing source/AI behavior unless requested.
option(MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE
    "Admit original GameAudio.Initialize in the selected frontend diagnostic" OFF)

function(mscharged_select_original_game_audio_initialize target)
    if(NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_OWNER OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE)
        message(FATAL_ERROR "Original audio initialization requires its original owner and frontend sequence")
    endif()
    get_target_property(_kind "${target}" TYPE)
    if(NOT _kind STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original audio initialization belongs to the existing isolated source module")
    endif()
    include(cmake/NativeAXFunctional.cmake)
    include(cmake/NativeThreadQueues.cmake)
    # Reuse the independently frozen402/410 selector in the SAME module.
    include(cmake/OriginalGameAudioInitProviders.cmake)
    mscharged_select_original_game_audio_init_providers("${target}")
    target_sources("${target}" PRIVATE tests/diagnostics/original_game_audio_initialize.cpp)
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE=1)
    target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C>:-fexceptions>")
    mscharged_set_original_module_exports("${target}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_game_audio_initialize_exports.map")
    target_sources(charged_original_main_credits_host PRIVATE
        tests/diagnostics/original_game_audio_hardware.cpp)
    target_compile_definitions(charged_original_main_credits_host PRIVATE
        MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_INITIALIZE=1)
    target_link_libraries(charged_original_main_credits_host PRIVATE
        charged_native_ax_functional charged_native_thread_queues)
    # The original image imports true host helpers only when first executed.
    # Force extraction into the sole ELF host; no module SDK is linked.
    foreach(_symbol IN ITEMS
            AIGetDMABytesLeft AIInit AIInitDMA AIRegisterDMACallback AIStartDMA
            OSCreateAlarm OSSetPeriodicAlarm OSInitThreadQueue OSWakeupThread
            OSDisableInterrupts OSRestoreInterrupts SCGetSoundMode
            DCFlushRange DCFlushRangeNoSync DCInvalidateRange
            WPADCanSendStreamData WPADControlSpeaker WPADSendStreamData)
        mscharged_require_original_host_symbol(charged_original_main_credits_host INTERFACE "${_symbol}")
    endforeach()
endfunction()
