include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER
    "Admit the original network task owner and initializer" OFF)

# Explicit selected-source frontend diagnostic. Default caller omits this helper.
# Requires the genuine original-main/frontend allocator/function-pool foundation
# and prepared 0473/0476/0477. This adds real source owners, not socket startup.
function(mscharged_add_original_frontend_network_owner_diagnostic target)
    if(NOT MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER)
        return()
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Network owner diagnostic requires its existing original frontend target")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY" OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TITLE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS)
        message(FATAL_ERROR "Original network initialization requires the original frontend session and input owners")
    endif()
    set(network_owner_sources
        "${MSCHARGED_PREPARED}/src/Game/Task/NetworkUpdateTask.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/SocketNetwork.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkMessageRegistry.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkMessages.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkGameStartMessage.cpp"
        "${MSCHARGED_PREPARED}/src/Game/InputRouter.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetTournManager.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkDraft.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkStatsManager.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkDraftMessages.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkSkipNisMessages.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkPauseMessages.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkMegaStrikeMessages.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkTournamentMessages.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkInput.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkSeasonCalendar.cpp"
        "${MSCHARGED_PREPARED}/src/Game/DetermDataEvent.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/CaptainSelectionOrder.cpp"
        "${MSCHARGED_PREPARED}/src/Game/SH/SHOnlineMatchmakingDraft.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/TransportMessage.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Task/ComUpdateTask.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetMessageAllInputs.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkSocket.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/ReliableSocket.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/TransportSocket.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkStats.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkSessionData.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkRandom.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/TransportConnection.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/TransportPacket.cpp"
        "${MSCHARGED_PREPARED}/src/NL/blowfish.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlBufferedWriter.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlAsyncFileBuffer.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkRandomSeed.cpp"
        "${MSCHARGED_PREPARED}/src/Game/LANLobby.cpp"
        "${MSCHARGED_PREPARED}/src/Game/NetworkLobby.cpp"
        "${MSCHARGED_PREPARED}/src/Game/LANConnectionMessages.cpp"
        "${PROJECT_SOURCE_DIR}/src/platform/transport_connection_address.cpp"
    )
    get_target_property(existing_sources "${target}" SOURCES)
    get_target_property(owner_source_dir "${target}" SOURCE_DIR)
    set(existing_absolute_sources)
    foreach(existing_source IN LISTS existing_sources)
        get_filename_component(existing_absolute "${existing_source}" ABSOLUTE BASE_DIR "${owner_source_dir}")
        list(APPEND existing_absolute_sources "${existing_absolute}")
    endforeach()
    foreach(source IN LISTS network_owner_sources)
        if(NOT source IN_LIST existing_absolute_sources)
            target_sources("${target}" PRIVATE "${source}")
        endif()
    endforeach()
    target_include_directories("${target}" PRIVATE "${MSCHARGED_PREPARED}/src/RVL_SDK")
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER=1)
endfunction()
