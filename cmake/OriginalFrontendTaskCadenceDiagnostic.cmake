include_guard(GLOBAL)

# Temporary source-task admission for the already selected284 Boot/Intro flow.
# The genuine nlTaskManager owns timing/order and MoviePlay after each task.
# Normal original sources, the Credits selection and old unarmed comparison
# remain unchanged with this option OFF. Full Async/3D/AX/reset remains held.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
    "Run selected original loading/begin/frontend/end tasks for Boot/Intro" OFF)
function(mscharged_select_original_frontend_task_cadence target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE OR NOT TARGET "${target}")
        message(FATAL_ERROR "Source task cadence requires the selected original frontend sequence module")
    endif()
    get_target_property(_task_type "${target}" TYPE)
    if(NOT _task_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original task providers must belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE=1)
    get_target_property(_task_existing "${target}" SOURCES)
    foreach(_task_source IN ITEMS
        src/Game/Task/LoadingTask.cpp
        src/Game/Task/BeginFrameTask.cpp
        src/Game/Task/FrontEndTask.cpp
        src/Game/Task/EndFrameTask.cpp
        src/NL/nlTask.cpp)
        set(_task_path "${MSCHARGED_PREPARED}/${_task_source}")
        if(NOT _task_path IN_LIST _task_existing)
            target_sources("${target}" PRIVATE "${_task_path}")
        endif()
    endforeach()
endfunction()
