include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# Temporary original Strikers 101/Challenge briefing integration.
# ChallengeSelectScene requests scene 77 (SHStrikerTimesChallenge); its
# Continue loads the authored challenge configuration and requests scene 78
# (SHChooseSides2 TOURNAMENT). The whole handlers own every request.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_BRIEFINGS
    "Admit original 101/Challenge briefings and tournament Choose Sides in the frontend diagnostic" OFF)

function(mscharged_select_original_frontend_briefings target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_BRIEFINGS)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_CHALLENGES
            MSCHARGED_DIAGNOSTIC_FRONTEND_DOMINATION)
        if(NOT ${_required})
            message(FATAL_ERROR "Original 101/Challenge briefings require ${_required}")
        endif()
    endforeach()
    get_target_property(_briefings_type "${target}" TYPE)
    if(NOT _briefings_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original briefings belong to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_BRIEFINGS=1)
    _mscharged_world_owned_sources("${target}" _briefings_known)
    foreach(_relative IN ITEMS
            src/Game/SH/SHStrikerTimesBase.cpp
            src/Game/SH/SHStrikerTimesChallenge.cpp
            # Genuine owners of data the handler's reached Game/FE code imports
            # at module load (gAIProfilingClock/gAIActivityClock, g_AllActorsHidden).
            src/Game/AI/AIContext.cpp
            src/Game/RenderSnapshot.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _briefings_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _briefings_known "${_source}")
        endif()
    endforeach()
endfunction()
