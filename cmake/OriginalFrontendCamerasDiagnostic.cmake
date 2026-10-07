include_guard(GLOBAL)

# Selected original camera dispatcher/update admission. The original script7
# must request its cameras after real prior services; this adds no main preload.
# Full script, world/effects, Intro/Title/menu and task runtime remain separate
# gates. Existing frontend behavior stays unchanged with this option OFF.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
    "Admit original camera services in the selected frontend diagnostic" OFF)
function(mscharged_select_original_frontend_cameras target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE)
        message(FATAL_ERROR "Camera admission requires the named frontend sequence and source task cadence")
    endif()
    get_target_property(_camera_type "${target}" TYPE)
    if(NOT _camera_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original camera providers belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS=1)
    get_target_property(_camera_existing "${target}" SOURCES)
    foreach(_camera_source IN ITEMS
        src/Game/Camera/CameraMan.cpp
        src/Game/Camera/animcam.cpp
        src/Game/Camera/BaseCam.cpp
        src/Game/Camera/DebugCam.cpp
        src/Game/Camera/rumblefilter.cpp
        src/Game/Camera/noisefilter.cpp
        src/Game/Camera/tu_800F9460.cpp
        src/Game/FE/feCamera.cpp
        src/Game/AI/AiUtil.cpp
        src/Game/objectblur.cpp
        src/Game/Task/FixedUpdateTask.cpp
        src/Game/Task/TweakerTask.cpp
        src/Game/CharacterTemplate.cpp
        src/Game/Render/depthoffield.cpp
        src/Game/Render/ShootToScoreMeter.cpp
        src/Game/Render/NetMesh.cpp
        src/Game/Render/NPCManager.cpp
        src/Game/FE/feManager.cpp
        src/Game/Physics.cpp
        src/Game/AI/FilteredRandom.cpp
        src/Game/AI/FuzzyRuntimeBase.cpp
        src/Game/AI/FuzzyVariant.cpp
        src/Game/AI/Variant.cpp
        src/Game/AI/Powerups.cpp
        src/Game/AI/tu_8030EDB0.cpp
        src/Game/AI/Fielder.cpp
        src/Game/EventDataTypes.cpp
        src/Game/Field.cpp
        src/Game/GameTweaksManager.cpp
        src/Game/InputManager.cpp
        src/Game/NetworkInputRecording.cpp
        src/Game/NetworkSync.cpp
        src/Game/Render/PeachPhoto.cpp
        src/Game/Physics/PhysicsEventQueue.cpp
        src/Game/HBMManager.cpp
        src/Game/Render/NumberDisplay.cpp
        src/Game/DetInput.cpp
        src/Game/Physics/PhysicsPatch.cpp
        src/Game/DebugWriteCache.cpp
        src/Game/Net.cpp
        src/Game/Render/ImpostorManager.cpp
        src/Game/UnidentifiedTweakAction.cpp
        src/NL/glx/glxMatrix.cpp)
        set(_camera_path "${MSCHARGED_PREPARED}/${_camera_source}")
        if(NOT _camera_path IN_LIST _camera_existing)
            target_sources("${target}" PRIVATE "${_camera_path}")
        endif()
    endforeach()
    if(NOT "src/platform/camera_data_transport.cpp" IN_LIST _camera_existing)
        target_sources("${target}" PRIVATE src/platform/camera_data_transport.cpp)
    endif()
endfunction()
