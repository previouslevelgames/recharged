include_guard(GLOBAL)
include(cmake/FriendCodeKeys.cmake)
include(cmake/Network.cmake)
include(cmake/OriginalModuleLinkage.cmake)
include(cmake/OnlineReports.cmake)
include(cmake/OriginalWorldOwners.cmake)
include(cmake/WiiQsort.cmake)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_ONLINE
    "Admit the original DWC, GameSpy, NHTTP and NET sources in the frontend module" OFF)

# The online SDK is original SDK source, so it joins the isolated game module
# like the flash providers: DWC reaches the module's own NAND/FS, already
# initialized by original main, and the module's OSMutex. The host supplies
# only native services (SO/NCD sockets and configuration, SC/NWC24/VF console
# identity, the disc game code). SSL stays unresolved until called, like the
# module's other unfinished functions: with the NAS reached over http://
# NHTTP never calls it.
function(mscharged_select_original_frontend_online target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_ONLINE)
        return()
    endif()
    # Main Menu hosts the Online item and checks MiiManager before it opens.
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_FLASH
            MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER
            MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU
            MSCHARGED_DIAGNOSTIC_FRONTEND_MII_RESOURCES)
        if(NOT ${_required})
            message(FATAL_ERROR "The original online SDK requires ${_required}")
        endif()
    endforeach()
    # The LAN lobby takes over the same Online item, so retail online would be
    # unreachable. Select one of them per build.
    if(MSCHARGED_DIAGNOSTIC_FRONTEND_LAN)
        message(FATAL_ERROR "MSCHARGED_DIAGNOSTIC_FRONTEND_ONLINE and MSCHARGED_DIAGNOSTIC_FRONTEND_LAN both take Main Menu's Online item; enable only one")
    endif()
    if(NOT TARGET charged_network)
        message(FATAL_ERROR "The original online SDK requires the POSIX SO backend (cmake/Network.cmake)")
    endif()
    get_target_property(_online_type "${target}" TYPE)
    if(NOT _online_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "The original online SDK belongs to the isolated original game module")
    endif()
    set(_online_sources
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_account.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_auth_interface.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_base64.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_common.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_encsession.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_error.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_friend.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_ghttp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_init.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwci_np_math.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_login.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_main.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_match.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_memfunc.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_nastime.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_nonport.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_ranking.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_ranksession.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_report.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_transport.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsAvailable.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsCrypt.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsLargeInt.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsRC4.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsSHA1.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsSSL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsXML.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/darray.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpBuffer.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpCallbacks.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpCommon.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpConnection.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpEncryption.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpMain.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpPost.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpProcess.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiBuddy.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiBuffer.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpi.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiCallback.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiConnect.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiInfo.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiOperation.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiPeer.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiProfile.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiSearch.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiTransfer.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiUnique.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiUtility.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gstats/gbucket.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gstats/gstats.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Auth.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Buffer.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Callback.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Connection.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Main.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Message.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Socket.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Utility.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/hashtable.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/md5c.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/natneg/NATify.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/natneg/natneg.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/nonport.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/qr2/qr2.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/qr2/qr2regkeys.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_crypt.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_queryengine.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_serverbrowsing.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_server.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_serverlist.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/d_nhttp.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/d_nhttp_common.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/d_nhttp_private.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_bgnend.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_control.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_list.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_os_RVL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_recvbuf.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_request.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_response.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_socket_RVL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_stdlib_RVL.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_thread.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/net/hmac.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/net/md5.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/net/neterrorcode.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/net/NETVersion.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/net/wireless_macaddr.c"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSMutex.c"
        # RFLGetFavoriteColor for the Mii selection; the Mii resources admission
        # does not compile the model unit.
        "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_Model.c"
        # The Mii icons of the online scenes (MiiManager::CreateIcon).
        "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/RFL_Icon.c")
    # Sources already in the module (OSMutex through the Mii resources) keep
    # their existing owner and options.
    _mscharged_world_owned_sources("${target}" _online_known)
    set(_online_added)
    foreach(_source IN LISTS _online_sources)
        if(NOT _source IN_LIST _online_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _online_added "${_source}")
        endif()
    endforeach()
    # GameSpy's arrays and the friend list sort with the Wii's qsort natively
    # (patch 0754).
    target_link_libraries("${target}" PRIVATE charged_wii_qsort)
    # Nothing outside the module calls the SDK, so every entry point is hidden
    # and --gc-sections drops those the game never reaches, as CodeWarrior's
    # linker did for the retail binary.
    set_property(SOURCE ${_online_added} TARGET_DIRECTORY "${target}" APPEND PROPERTY
        COMPILE_OPTIONS -fvisibility=hidden -Wno-unused-value
        -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast
        -Werror=implicit-function-declaration)
    set_property(SOURCE ${_online_added} TARGET_DIRECTORY "${target}" APPEND PROPERTY
        COMPILE_DEFINITIONS _REVOLUTION=1)
    set_property(SOURCE ${_online_added} TARGET_DIRECTORY "${target}" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${MSCHARGED_PREPARED}/src/RVL_SDK"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP")
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_FRONTEND_ONLINE=1)
    # The icons draw the Mii's model and face texture: RFL_MakeTex.c and the
    # transports of their Wii-serialized resource data.
    mscharged_add_original_rfl_shape("${target}")

    # The retail online scenes, from the Mii selection to the drafts. The LAN
    # admission compiles the lobby and draft scenes only when it is selected.
    foreach(_relative IN ITEMS
            src/Game/SH/SHNetworkStart.cpp
            src/Game/SH/SHOnlineConnectionQuality.cpp
            src/Game/SH/SHOnlineFriendCodeEntry.cpp
            src/Game/SH/SHOnlineFriends.cpp
            src/Game/SH/SHOnlineFriendsChooseSides.cpp
            src/Game/SH/SHOnlineFriendsDraft.cpp
            src/Game/SH/SHOnlineGuestControllerSelect.cpp
            src/Game/SH/SHOnlineHub.cpp
            src/Game/SH/SHOnlineInvitePlayers.cpp
            src/Game/SH/SHOnlineInvitePreview.cpp
            src/Game/SH/SHOnlineInviteResponse.cpp
            src/Game/SH/SHOnlineInviteStatus.cpp
            src/Game/SH/SHOnlineMiiSelect.cpp
            src/Game/SH/SHOnlineMiiSelectOverlay.cpp
            src/Game/SH/SHOnlinePlayerCount.cpp
            src/Game/SH/SHOnlineRanking.cpp
            src/Game/FE/feOnlinePlayerRow.cpp
            src/Game/FE/OnlineRanking.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _online_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _online_known "${_source}")
        endif()
    endforeach()
    # These TUs format and copy Wii16 L"%d"/L"%ls" literals through the
    # original 16-bit formatter; none of them makes host wide-character calls.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        foreach(_online_wii16 IN ITEMS
                src/Game/SH/SHOnlineFriendCodeEntry.cpp
                src/Game/SH/SHOnlineFriendsChooseSides.cpp
                src/Game/SH/SHOnlineHub.cpp
                src/Game/SH/SHOnlineInvitePreview.cpp
                src/Game/SH/SHOnlineMiiSelect.cpp
                src/Game/SH/SHOnlineMiiSelectOverlay.cpp
                src/Game/SH/SHOnlineRanking.cpp)
            set_property(SOURCE "${MSCHARGED_PREPARED}/${_online_wii16}"
                APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
        endforeach()
    endif()

    target_link_libraries(charged_original_game_host PRIVATE
        charged_network charged_native_system_settings charged_friend_code_keys)
    # Native services the SDK imports that no other admission already pulls
    # into the host.
    foreach(_symbol IN ITEMS
            OSGetAppGamename
            SCGetProductArea SCGetProductCode SCGetProductSN
            NWC24OpenLib NWC24GetMyUserId NWC24CloseLib
            VFInit VFIsAvailable VFFinalize
            NCDGetCurrentIfConfig NCDGetCurrentIpConfig NCDiGetWirelessMacAddress
            NCDiGetEnabledConfigList)
        mscharged_require_original_host_symbol(charged_original_game_host INTERFACE "${_symbol}")
    endforeach()
    # The friend code entry's number keys (patch 0756).
    mscharged_require_original_host_symbol(charged_original_game_host INTERFACE MSChargedTakeFriendCodeKey)
    target_compile_definitions(charged_original_game_host PRIVATE
        MSCHARGED_HAS_ONLINE_SERVER_OVERRIDE=1)
    # The WFC server's match reports, sent to the same server.
    target_link_libraries(charged_original_game_host PRIVATE charged_online_reports)
endfunction()
