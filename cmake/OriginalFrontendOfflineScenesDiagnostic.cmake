include_guard(GLOBAL)

# Temporary admission of the remaining original offline scene factory cases
# (cup cheat, legal, movie player, NLG logo, Hall of Fame profile, health
# warning, Striker Times, demo and loading overlays). Scripts and handlers decide
# when they appear; the original classes run unchanged. Online scenes stay gated.
# Follows the Hall of Fame admission, so existing frontend builds pick it up.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_OFFLINE_SCENES
    "Admit the remaining original offline scene factory cases"
    ${MSCHARGED_DIAGNOSTIC_FRONTEND_HALL_OF_FAME})

function(mscharged_select_original_frontend_offline_scenes target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_OFFLINE_SCENES)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_HALL_OF_FAME
            MSCHARGED_DIAGNOSTIC_FRONTEND_MATCH_LOADING
            MSCHARGED_DIAGNOSTIC_FRONTEND_CREDITS)
        if(NOT ${_required})
            message(FATAL_ERROR "Original offline scene admission requires ${_required}")
        endif()
    endforeach()
    get_target_property(_offline_type "${target}" TYPE)
    if(NOT _offline_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original offline scenes belong to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_OFFLINE_SCENES=1)
    get_target_property(_offline_existing "${target}" SOURCES)
    get_target_property(_offline_source_dir "${target}" SOURCE_DIR)
    set(_offline_absolute)
    foreach(_source IN LISTS _offline_existing)
        get_filename_component(_absolute "${_source}" ABSOLUTE
            BASE_DIR "${_offline_source_dir}")
        list(APPEND _offline_absolute "${_absolute}")
    endforeach()
    foreach(_relative IN ITEMS
            src/Game/SH/SHCupCheater.cpp
            src/Game/SH/SHMoviePlayer.cpp
            src/Game/FE/SHCrossFader.cpp
            src/Game/SH/SHHallOfFameRoom.cpp
            src/Game/FE/Overlay/OverlayHandlerStrikerTimes.cpp
            src/Game/FE/Overlay/OverlayHandlerDemo.cpp)
        set(_offline_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _offline_source IN_LIST _offline_absolute)
            target_sources("${target}" PRIVATE "${_offline_source}")
            list(APPEND _offline_absolute "${_offline_source}")
        endif()
    endforeach()
endfunction()
