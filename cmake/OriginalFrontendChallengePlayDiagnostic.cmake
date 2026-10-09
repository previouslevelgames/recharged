include_guard(GLOBAL)

# Temporary admission of original Strikers 101 lesson and Striker Challenge play
# (decomp patch 0620 onward): the 101 pause, lesson movie player, Challenge
# preview and the outcome pages these modes reach. Original handlers, scripts
# and game state own every objective, result, unlock, save and transition.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_CHALLENGE_PLAY
    "Admit original 101 lesson and Striker Challenge play, outcomes and retry" OFF)

function(mscharged_select_original_frontend_challenge_play target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CHALLENGE_PLAY)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_BRIEFINGS
            MSCHARGED_DIAGNOSTIC_FRONTEND_MATCH_LOADING)
        if(NOT ${_required})
            message(FATAL_ERROR "Original lesson and Challenge play requires ${_required}")
        endif()
    endforeach()
    get_target_property(_challenge_type "${target}" TYPE)
    if(NOT _challenge_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original lesson and Challenge play belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_CHALLENGE_PLAY=1)
    # The Challenge preview formats its initial time and score with Wii16
    # L"..." literals through the original 16-bit formatter; the unit makes no
    # host wide-character calls and no project header in it uses wchar_t.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        set_property(SOURCE "${MSCHARGED_PREPARED}/src/Game/FE/Overlay/OverlayHandlerChallengePreview.cpp"
            APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
    endif()
endfunction()
