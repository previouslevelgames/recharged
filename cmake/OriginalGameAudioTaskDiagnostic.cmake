include_guard(GLOBAL)

# Admit the existing source task/declaration only after genuine initialization.
# Main remains the sole owner; no host tick or alternate task body is supplied.
option(MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK
    "Admit the original priority15 audio task in the selected frontend diagnostic" OFF)

function(mscharged_select_original_game_audio_task target)
    if(NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE)
        message(FATAL_ERROR "The source audio task requires original audio initialization and task cadence")
    endif()
    get_target_property(_kind "${target}" TYPE)
    if(NOT _kind STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "The original audio task belongs to its isolated main module")
    endif()
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK=1)
    target_compile_definitions(charged_original_main_credits_host PRIVATE
        MSCHARGED_HAS_ORIGINAL_GAME_AUDIO_TASK=1)
    # Host must observe actual owner+initialized+config-loaded flags==7 through
    # charged_original_audio_observe after main and before the FIRST frame.
    # Do not guard/replace the original Run body or manually update GameAudio.
endfunction()
