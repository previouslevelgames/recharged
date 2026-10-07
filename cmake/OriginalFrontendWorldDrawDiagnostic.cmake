include_guard(GLOBAL)

# Temporary owner-authorized diagnostic. Original primary-camera setup and
# frontend world calls execute in their source positions. Full original
# secondary setup uses the genuine NIS owner; active playback remains separate.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD_DRAW
    "Admit original frontend world drawing and full camera matrix setup" OFF)

function(mscharged_select_original_frontend_world_draw target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD_DRAW)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_MATERIALS
            MSCHARGED_DIAGNOSTIC_FRONTEND_NIS_OWNER
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE)
        if(NOT ${_required})
            message(FATAL_ERROR "Original frontend world draw requires ${_required}")
        endif()
    endforeach()
    if(NOT TARGET "${target}_world_views")
        message(FATAL_ERROR "Original world draw requires its actual full world-view owner cohort")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD_DRAW=1)
    # UpdateGameObjectLighting reads the genuine original character static.
    # Admit its whole source owner without constructing or updating characters.
    _mscharged_world_owned_sources("${target}" _known)
    foreach(_relative IN ITEMS
            src/Game/Drawable/DrawableCharacter.cpp
            src/Game/GL/GLFourTextureAddMeshWriter.cpp
            src/Game/GL/GLWarbleMeshWriter.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _known)
            target_sources("${target}" PRIVATE "${_source}")
        endif()
    endforeach()
endfunction()
