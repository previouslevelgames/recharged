include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# Temporary original Road to Striker Cup frontend integration. Main Menu's cup
# item starts or resumes a cup through feCupFlow; the authored transitions push
# Striker Cup captain/sidekick setup (6/7), the cup news (39) and the hub (31),
# whose Schedule, Cup Stats, Rules and Play buttons reach the group schedule (32),
# standings/awards pages (36-38) and Cup Choose Sides (8); played matchups open
# game results (33), and later phases cycle the knockout (34) and final-round
# (35) pages. The whole handlers and CupManager own every request.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_CUP
    "Admit original Road to Striker Cup setup, hub and detail pages in the frontend diagnostic" OFF)

function(mscharged_select_original_frontend_cup target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CUP)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_DOMINATION)
        if(NOT ${_required})
            message(FATAL_ERROR "Original Road to Striker Cup frontend requires ${_required}")
        endif()
    endforeach()
    get_target_property(_cup_type "${target}" TYPE)
    if(NOT _cup_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original Road to Striker Cup frontend belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_CUP=1)
    _mscharged_world_owned_sources("${target}" _cup_known)
    foreach(_relative IN ITEMS
            src/Game/SH/SHStrikerTimesBase.cpp
            src/Game/SH/SHCupNews.cpp
            src/Game/SH/SHRoadToStrikersCupHub.cpp
            src/Game/SH/SHCupHub.cpp
            src/Game/SH/SHCupKnockout.cpp
            src/Game/SH/SHCupFinalRounds.cpp
            src/Game/SH/SHStrikerCupStandings.cpp
            src/Game/SH/SHStrikerCupAwards.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _cup_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _cup_known "${_source}")
        endif()
    endforeach()
endfunction()
