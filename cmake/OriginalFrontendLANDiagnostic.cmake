include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# Temporary original LAN lobby integration. Retail never calls
# NetworkSession::InitializeLAN; under this admission Main Menu's Online item
# starts the LAN session and requests the original lobby (scene 24,
# NetworkStartScene) instead of the DWC login. Its Create/Join run the original
# LANLobby; a launched game reaches the original network draft (the matchmaking
# or friends draft scene, then the Domination captain choice). Hub, login, Mii
# selection and every DWC scene keep their gates.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_LAN
    "Admit the original LAN lobby and network draft from Main Menu's Online item" OFF)

function(mscharged_select_original_frontend_lan target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_LAN)
        return()
    endif()
    # The network owner compiles the session, sockets and LANLobby; Main Menu
    # hosts the Online item; the draft ends in the Domination captain choice.
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER
            MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU
            MSCHARGED_DIAGNOSTIC_FRONTEND_DOMINATION)
        if(NOT ${_required})
            message(FATAL_ERROR "The original LAN lobby requires ${_required}")
        endif()
    endforeach()
    if(NOT TARGET charged_network)
        message(FATAL_ERROR "The original LAN lobby requires the POSIX SO backend (cmake/Network.cmake)")
    endif()
    get_target_property(_lan_type "${target}" TYPE)
    if(NOT _lan_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "The original LAN lobby belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_LAN=1)
    # Sources already in the module (directly or through object libraries) are
    # skipped, so an earlier admission that owns one of these does not compile
    # it twice.
    _mscharged_world_owned_sources("${target}" _lan_known)
    foreach(_relative IN ITEMS
            src/Game/SH/SHNetworkStart.cpp
            src/Game/SH/SHOnlineFriendsDraft.cpp
            # InitializeOnlineMatch, which prepares the match for the lobby
            src/Game/SH/SHOnlinePlayerCount.cpp
            # UpdateOnlinePlayerRow, which both draft scenes call when created
            src/Game/FE/feOnlinePlayerRow.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _lan_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _lan_known "${_source}")
        endif()
    endforeach()
endfunction()
