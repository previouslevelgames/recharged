include_guard(GLOBAL)

# Constructor/resource ownership only. The original main owns this statement
# and the constructor owns the five original NL requests. Playback, secondary
# camera FOV and BeginFrame SetupMatrices remain separate source prerequisites.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_NIS_OWNER
    "Admit the original NIS constructor in the selected frontend diagnostic" OFF)
function(mscharged_select_original_frontend_nis_owner target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_NIS_OWNER)
        return()
    endif()
    if(NOT "${target}" STREQUAL "mscharged_original_frontend_module")
        message(FATAL_ERROR "NIS owner admission is scoped to the selected frontend module")
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS)
        message(FATAL_ERROR "NIS owner admission requires original frontend sequence and camera providers")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original NIS providers require the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_NIS_OWNER=1)
    # NisPlayer includes its actual upstream interpreter/loading source parts
    # through source-root-relative paths. Keep those whole parent TUs intact.
    target_include_directories("${target}" PRIVATE "${MSCHARGED_PREPARED}")
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN ITEMS
            src/Game/NisPlayer.cpp
            src/Game/Render/Nis.cpp
            src/Game/Render/NisPlayerOverlay.cpp
            src/Game/Transitions/ModelTransition.cpp
            src/Game/Blinker.cpp
            src/Game/Effects/ParticleSystem.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
        endif()
    endforeach()
    # Actual SimpleParser, interpreter, NL, allocator, event, camera and GL
    # providers already belong to this module. No SDK instance or new loader.
endfunction()
