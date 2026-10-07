include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_FIXED_INPUT
    "Admit original main fixed update and input prerequisites" OFF)

# Default-off subordinate frontend admission; requires genuine source pads,
# network481 and Presentation487 at their existing original-main positions.
# Adds no frame loop and leaves source priorities, masks and decisions intact.
function(mscharged_add_original_frontend_fixed_input_diagnostic target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_FIXED_INPUT)
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
        message(FATAL_ERROR "Fixed input diagnostic requires its existing original frontend target")
    endif()
    set(source "${MSCHARGED_PREPARED}/src/Game/Pad/FlickDetection.cpp")
    get_target_property(existing_sources "${target}" SOURCES)
    get_target_property(owner_source_dir "${target}" SOURCE_DIR)
    set(existing_absolute_sources)
    foreach(existing_source IN LISTS existing_sources)
        get_filename_component(existing_absolute "${existing_source}" ABSOLUTE BASE_DIR "${owner_source_dir}")
        list(APPEND existing_absolute_sources "${existing_absolute}")
    endforeach()
    if(NOT source IN_LIST existing_absolute_sources)
        target_sources("${target}" PRIVATE "${source}")
    endif()
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_FRONTEND_FIXED_INPUT=1)
endfunction()
