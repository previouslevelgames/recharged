include_guard(GLOBAL)

# Temporary source-factory admission after genuine Intro movie abort.
# Natural presentation/camera waits and later MainMenu are not completed here.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_TITLE
    "Admit original Title in the bounded original frontend sequence" OFF)
function(mscharged_select_original_frontend_title target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TITLE)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE)
        message(FATAL_ERROR "Title admission requires the named original frontend sequence")
    endif()
    get_target_property(_title_type "${target}" TYPE)
    if(NOT _title_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Title providers belong to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_TITLE=1)
    get_target_property(_title_existing "${target}" SOURCES)
    foreach(_title_source IN ITEMS
        src/Game/SH/SHTitleScreen.cpp
        src/Game/FE/feMusic.cpp
        src/Game/FE/fePopupMenu.cpp
        src/Game/TrophyInfo.cpp
        src/Game/SH/SHOnlineLogin.cpp
        src/Game/DB/CupManager.cpp
        src/Game/FriendManager.cpp
        src/Game/DB/StrikerChallenge.cpp
        src/Game/NetworkSession.cpp
        src/Game/NetworkDebug.cpp
        src/Game/DB/StatsTracker.cpp)
        set(_title_path "${MSCHARGED_PREPARED}/${_title_source}")
        if(NOT _title_path IN_LIST _title_existing)
            target_sources("${target}" PRIVATE "${_title_path}")
        endif()
    endforeach()
    # Original SaveLoad/Wii16/TPL policy is already selected by the sequence.
    # Keep the genuine GameInfo banner owner and all Title icon/VI requests.
endfunction()
