include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_FINALIZE
    "Admit original initial frontend finalization" OFF)

function(mscharged_select_original_frontend_finalize target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_FINALIZE)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_MII_RESOURCES
            MSCHARGED_DIAGNOSTIC_MAIN_PRESENTATION_OWNER
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_NIS_OWNER)
        if(NOT ${_required})
            message(FATAL_ERROR "Original frontend finalization requires ${_required}")
        endif()
    endforeach()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_FINALIZE=1)
    # The existing original manager owns its instance and resource requests.
    # Keep its object, character, model and sprite methods in the same module.
    target_sources("${target}" PRIVATE
        "${MSCHARGED_PREPARED}/src/Game/Render/Impostor.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/ImpostorCharacter.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/ImpostorModel.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/ImpostorSprite.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/ImpostorCluster.cpp")
endfunction()
