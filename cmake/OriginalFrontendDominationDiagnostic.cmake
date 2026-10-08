include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# Temporary admission of the two original offline Domination selection pages.
# The authored MainMenu script owns the transition and real E3 selection.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_DOMINATION
    "Admit original offline Domination captain and sidekick selection" OFF)

function(mscharged_select_original_frontend_domination target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_DOMINATION)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER)
        if(NOT ${_required})
            message(FATAL_ERROR "Original Domination selection requires ${_required}")
        endif()
    endforeach()
    get_target_property(_domination_type "${target}" TYPE)
    if(NOT _domination_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original Domination selection belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_DOMINATION=1)
    _mscharged_world_owned_sources("${target}" _domination_known)
    foreach(_relative IN ITEMS
            src/Game/SH/SHChooseCaptains.cpp
            src/Game/SH/SHChooseSidekicks.cpp
            src/Game/FE/feCharacterPDAComponent.cpp
            src/Game/FE/feCaptainComponent.cpp
            src/Game/FE/feScrollBar.cpp
            src/Game/FE/feScrollText.cpp
            src/Game/FE/feTimer.cpp
            src/Game/SAnim/pnBlender.cpp
            src/Game/SH/OnlineGameInfo.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _domination_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _domination_known "${_source}")
        endif()
    endforeach()
endfunction()
