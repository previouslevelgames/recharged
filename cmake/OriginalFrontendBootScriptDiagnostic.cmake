include_guard(GLOBAL)

# Selected owned function0, not the preceding BootLoadingToFE or the complete
# CleanBootShutdown route. Main must exclude its earlier diagnostic FE/font/Boot
# preloads and use the original NL bytecode callback and source VM APIs.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_SCRIPT
    "Admit the authored original async script0 Boot/font/Splash source route" OFF)

function(mscharged_select_original_frontend_boot_script target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_SCRIPT)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_AUDIO OR
            NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE OR
            NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK)
        message(FATAL_ERROR "Authored Boot script0 requires the selected sequence, original audio initialization/task and Splash service admission")
    endif()
    get_target_property(_kind "${target}" TYPE)
    if(NOT _kind STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original Boot script providers must share the existing isolated source module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_BOOT_SCRIPT=1)
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN ITEMS
            src/Game/AsyncLoading.cpp
            src/Game/InterpreterCore.cpp
            src/Game/InterpreterOperations.cpp
            src/Game/Font/FontLoading.cpp
            src/Game/Font/fontmanager.cpp
            src/NL/nlLocalization.cpp
            src/Game/FE/feResourceManager.cpp
            src/Game/FE/feSceneManager.cpp
            src/Game/FE/BaseGameSceneManager.cpp
            src/Game/FE/GameSceneManager.cpp
            src/Game/SH/SHBootLoading.cpp
            src/Game/FE/FEAudio.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _existing "${_path}")
        endif()
    endforeach()
    if(NOT "src/platform/vm_address_abi.cpp" IN_LIST _existing)
        target_sources("${target}" PRIVATE src/platform/vm_address_abi.cpp)
    endif()
    target_sources("${target}" PRIVATE tests/diagnostics/original_frontend_boot_script.cpp)
    # Reuse the original audio initializer's same export map. The focused
    # observer additions preserve genuine idle/bank-retirement exports416.
    set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_game_audio_initialize_exports.map")
    target_compile_definitions(charged_original_main_credits_host PRIVATE
        MSCHARGED_HAS_ORIGINAL_FRONTEND_BOOT_SCRIPT=1)
    # Original allocator/completed-byte domains, pools, managers, GX resources,
    # NL file services and InitPads remain existing source-owned prerequisites.
    # The helper neither constructs owners nor requests a file or plays a cue.
endfunction()
