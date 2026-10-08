include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# Temporary admission of the original FrontEndTask FE model owners
# (FEModelManager::Update/Render) and the FEModelManager-only presentation
# services (decomp patch 0552). Authored scripts and handlers own every model
# request; impostor/skin/animation providers are the whole original units.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_FE_MODELS
    "Admit original FE model owners and their presentation services" OFF)

function(mscharged_select_original_frontend_fe_models target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_FE_MODELS)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_FINALIZE)
        if(NOT ${_required})
            message(FATAL_ERROR "Original FE model owners require ${_required}")
        endif()
    endforeach()
    get_target_property(_fe_models_type "${target}" TYPE)
    if(NOT _fe_models_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original FE model owners belong to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_FE_MODELS=1)
    _mscharged_world_owned_sources("${target}" _fe_models_known)
    # FinishLoadModel/Render reach skinned animation, impostor sprite meshes,
    # impostor lighting and UpdateImpostorPositions (in the crowd unit, whose
    # manager/model-collection owners complete its link closure).
    foreach(_relative IN ITEMS
            src/Game/Render/SkinAnimatedNPC.cpp
            src/Game/GL/GLCompactColourMeshWriter.cpp
            src/Game/Render/ImpostorLighting.cpp
            src/Game/Render/ImpostorLightingColour.cpp
            src/Game/Render/CrowdImpostors.cpp
            src/Game/Render/CrowdImpostorManager.cpp
            src/Game/Render/CrowdModelCollection.cpp
            # SkinAnimatedNPC reaches RenderProjectedShadow, whose debug-only
            # g_bShadowBounds branch imports the g_ShapeRenderer object (data
            # binds at load). Its whole unit and writers provide it; original
            # main's Initialize stays omitted with the world/debug views.
            src/Game/Debug/ShapeRender.cpp
            src/Game/GL/MeshWriter.cpp
            src/Game/GL/GLColourMeshWriter.cpp
            # Character models render GXCharacterDamage, whose render unit calls
            # this Matching TEV helper.
            src/NL/glx/glxCharacterDamage.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _fe_models_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _fe_models_known "${_source}")
        endif()
    endforeach()
endfunction()
