include_guard(GLOBAL)

# Source-owned CleanBoot entry after the real prior BootLoadingToFE sequence.
# FEworld/service44 and later Mii/Intro/frontend-state transitions remain held.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_CLEAN_BOOT
    "Admit the original CleanBoot transition and FE pool/bank endpoints" OFF)

function(mscharged_select_original_frontend_clean_boot target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CLEAN_BOOT)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_TO_FE
            MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_SCRIPT
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_HBM
            MSCHARGED_DIAGNOSTIC_FRONTEND_AUDIO
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK)
        if(NOT ${_required})
            message(FATAL_ERROR "CleanBoot source admission requires ${_required}")
        endif()
    endforeach()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "CleanBoot must share the existing isolated source module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_CLEAN_BOOT=1)
    # The original frontend camera update queries this source-owned predicate.
    get_target_property(_sources "${target}" SOURCES)
    set(_profiler "${MSCHARGED_PREPARED}/src/Game/Task/ProfilerTask.cpp")
    if(NOT _profiler IN_LIST _sources)
        target_sources("${target}" PRIVATE "${_profiler}")
    endif()
    # Existing source managers/tasks and the current genuine bank/camera/HBM
    # providers own every request; no additional SDK provider is added here.
endfunction()
