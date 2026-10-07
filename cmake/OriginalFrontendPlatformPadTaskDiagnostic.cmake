include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_PLAT_PAD_TASK
    "Admit original platform pad task and registration" OFF)

# Default-off subordinate admission. Source main owns the sole task static and
# its original priority5/all-state registration; this helper runs no task/tick.
function(mscharged_add_original_frontend_platform_pad_task_diagnostic target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_PLAT_PAD_TASK)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
            MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER
            MSCHARGED_DIAGNOSTIC_MAIN_PRESENTATION_OWNER)
        if(NOT ${_required})
            message(FATAL_ERROR "Original input task admission requires ${_required}")
        endif()
    endforeach()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Platform pad task diagnostic requires its existing original frontend target")
    endif()
    get_target_property(existing_sources "${target}" SOURCES)
    get_target_property(owner_source_dir "${target}" SOURCE_DIR)
    set(existing_absolute_sources)
    foreach(existing_source IN LISTS existing_sources)
        get_filename_component(existing_absolute "${existing_source}" ABSOLUTE BASE_DIR "${owner_source_dir}")
        list(APPEND existing_absolute_sources "${existing_absolute}")
    endforeach()
    foreach(relative_source IN ITEMS
            src/Game/Task/PlatPadUpdateTask.cpp
            src/Game/RumbleActions.cpp)
        set(source "${MSCHARGED_PREPARED}/${relative_source}")
        if(NOT source IN_LIST existing_absolute_sources)
            target_sources("${target}" PRIVATE "${source}")
        endif()
    endforeach()
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_FRONTEND_PLAT_PAD_TASK=1)
endfunction()
