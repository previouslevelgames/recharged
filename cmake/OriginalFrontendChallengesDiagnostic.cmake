include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# Temporary original Tutorial/Challenges listing and Back integration.
# The authored MainMenu transition and whole handler own all requests.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_CHALLENGES
    "Admit original Tutorial and Challenges listings in the frontend diagnostic" OFF)

function(mscharged_select_original_frontend_challenges target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CHALLENGES)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SAVE_OWNERS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS)
        if(NOT ${_required})
            message(FATAL_ERROR "Original Challenges listing requires ${_required}")
        endif()
    endforeach()
    get_target_property(_challenges_type "${target}" TYPE)
    if(NOT _challenges_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original Challenges listings belong to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_CHALLENGES=1)
    _mscharged_world_owned_sources("${target}" _challenges_known)
    foreach(_relative IN ITEMS
            src/Game/SH/SHChallengeSelect.cpp
            src/Game/FE/feScrollBar.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _challenges_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _challenges_known "${_source}")
        endif()
    endforeach()
    # Preserve the two original Wii16 L"%d" literals and actual formatter.
    # Other TUs retain their existing native wchar width.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        set_property(SOURCE "${MSCHARGED_PREPARED}/src/Game/SH/SHChallengeSelect.cpp"
            APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
    endif()
endfunction()
