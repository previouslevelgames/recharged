include_guard(GLOBAL)

# Original prior stage: BootLoadingToFE -> script6 -> script14 -> MovieInit.
# This selector does not prove successful resources or complete CleanBoot.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_TO_FE
    "Admit the original prior BootLoadingToFE effects/NPC/movie stage" OFF)

function(mscharged_select_original_frontend_boot_to_fe target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_TO_FE)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_SCRIPT OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE OR
            NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE OR
            NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK)
        message(FATAL_ERROR "Prior Boot stage requires the original bytecode/task owner and genuine GameAudio initialization/task")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original prior Boot providers must share the isolated source module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_TO_FE=1)
    get_target_property(_sources "${target}" SOURCES)
    foreach(_source IN ITEMS
            src/Game/Effects/EmissionController.cpp
            src/Game/Effects/EmissionManager.cpp
            src/Game/Effects/EmitterCallbacks.cpp
            src/Game/Effects/EffectsBundleData.cpp
            src/Game/Effects/EffectsTemplate.cpp
            src/Game/Effects/EffectsGroup.cpp
            src/Game/Effects/ParticleSystem.cpp
            src/Game/Task/ParticleUpdateTask.cpp
            src/NL/plat/nlFileCache.cpp
            src/Game/Render/NPCManager.cpp
            src/Game/Task/FixedUpdateTask.cpp
            src/Game/Sys/movie.cpp
            src/NL/glx/glxLoadModel.cpp
            src/Game/SHierarchy.cpp
            src/Game/SAnim.cpp
            src/Game/PoseAccumulator.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
    endforeach()
    foreach(_native IN ITEMS
            src/platform/effects_data_abi.cpp
            src/platform/hierarchy_data_transport.cpp
            src/platform/sanim_data_transport.cpp)
        set(_path "${PROJECT_SOURCE_DIR}/${_native}")
        if(NOT _path IN_LIST _sources AND NOT _native IN_LIST _sources)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _sources "${_path}")
        endif()
    endforeach()
    target_compile_definitions(charged_original_main_credits_host PRIVATE
        MSCHARGED_HAS_ORIGINAL_FRONTEND_BOOT_TO_FE=1)
    # Literal original main owns Particle/Emission/cache initialization and
    # task registration. Source LoadingTask StateTransition requests the hashed
    # sequence, and Run retains the actual waits/results/next-state request.
    # Existing selected cadence rejects later unqualified state transitions.
    # The runtime must retain the source module/arenas at that terminal hold:
    # persistent effects/NPC/pool owners have no qualified full cleanup here.
    # Original CleanBoot/scripts13/7 and normal main remain separate gates.
endfunction()
