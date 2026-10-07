include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# The original CleanBoot script owns the world request, asynchronous waits and
# completion. Later Mii, frontend state transitions and world rendering remain
# separate integration steps.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD
    "Admit original CleanBoot frontend world loading" OFF)

function(mscharged_select_original_frontend_world target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_CLEAN_BOOT
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_MATERIALS
            MSCHARGED_DIAGNOSTIC_FRONTEND_NIS_OWNER)
        if(NOT ${_required})
            message(FATAL_ERROR "Original frontend world loading requires ${_required}")
        endif()
    endforeach()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD=1)
    mscharged_add_original_world_owners("${target}")
endfunction()
