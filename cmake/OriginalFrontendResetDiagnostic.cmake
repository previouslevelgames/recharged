include_guard(GLOBAL)

# The original task owns close, fade, source flush, black/retrace and the
# SDK shutdown sequence.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_RESET
    "Admit the original reset task with qualified native power removal" OFF)

function(mscharged_select_original_frontend_reset target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_RESET)
        return()
    endif()
    foreach(_required IN ITEMS MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_FLASH
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK)
        if(NOT ${_required})
            message(FATAL_ERROR "Original reset admission requires ${_required}")
        endif()
    endforeach()
    get_target_property(_kind "${target}" TYPE)
    if(NOT _kind STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original reset software belongs to the existing source module")
    endif()
    # Only main uses this admission macro. Keep it specific to this module's
    # compilation of main; the standalone Credits module has no reset owner.
    set_property(TARGET "${target}" PROPERTY MSCHARGED_ORIGINAL_RESET_OWNER TRUE)
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/Game/main.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS
        "$<$<BOOL:$<TARGET_PROPERTY:MSCHARGED_ORIGINAL_RESET_OWNER>>:MSCHARGED_DIAGNOSTIC_FRONTEND_RESET=1>")
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN ITEMS src/Game/Task/ResetTask.cpp
            src/RVL_SDK/os/OSAudioSystem.c)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
        endif()
    endforeach()
    # Source SDK tick deltas use signed32 wrap arithmetic under MWCC.
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSAudioSystem.c"
        APPEND PROPERTY COMPILE_OPTIONS -fwrapv)
    target_compile_definitions(charged_original_main_credits_host PRIVATE
        MSCHARGED_HAS_ORIGINAL_FRONTEND_RESET=1)
    target_link_libraries(charged_original_main_credits_host PRIVATE
        charged_native_os_shutdown_requests)
    foreach(_symbol IN ITEMS __OSShutdownToSBY __OSHotReset OSGetAppType
            __OSReboot __OSLaunchMenu __OSRelaunchTitle
            __VISetRGBModeImm __PADDisableRecalibration
            ChargedOSAudioDSPRead ChargedOSAudioDSPReadPair ChargedOSAudioDSPWrite)
        mscharged_require_original_host_symbol(charged_original_main_credits_host
            INTERFACE "${_symbol}")
    endforeach()
endfunction()
