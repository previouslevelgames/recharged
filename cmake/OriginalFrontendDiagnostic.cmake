include_guard(GLOBAL)

# The game modules: the shared template's sources and settings plus the
# frontend selections below. The host loads one of them for a given run.
include(cmake/OriginalFrontendSequenceDiagnostic.cmake)
include(cmake/OriginalFrontendTaskCadenceDiagnostic.cmake)
include(cmake/OriginalFrontendTitleDiagnostic.cmake)
include(cmake/OriginalFrontendCamerasDiagnostic.cmake)
include(cmake/OriginalFrontendFlashDiagnostic.cmake)
include(cmake/OriginalFrontendSaveOwnersDiagnostic.cmake)
include(cmake/OriginalFrontendHBMDiagnostic.cmake)
include(cmake/OriginalFrontendHOMEDiagnostic.cmake)
include(cmake/OriginalFrontendMaterialsDiagnostic.cmake)
include(cmake/OriginalSHMenuDiagnostic.cmake)
include(cmake/OriginalGameAudioOwnerDiagnostic.cmake)
include(cmake/OriginalGameAudioInitializeDiagnostic.cmake)
include(cmake/OriginalGameAudioTaskDiagnostic.cmake)
include(cmake/OriginalFrontendNisOwnerDiagnostic.cmake)
include(cmake/OriginalFrontendAudioDiagnostic.cmake)
include(cmake/OriginalFrontendBootScriptDiagnostic.cmake)
include(cmake/OriginalFrontendBootToFEDiagnostic.cmake)
include(cmake/OriginalFrontendCleanBootDiagnostic.cmake)
include(cmake/OriginalFrontendWorldDiagnostic.cmake)
include(cmake/OriginalFrontendWorldDrawDiagnostic.cmake)
include(cmake/OriginalFrontendNetworkOwnerDiagnostic.cmake)
include(cmake/OriginalFrontendLANDiagnostic.cmake)
include(cmake/OriginalOnline.cmake)
include(cmake/OriginalFrontendPresentationOwnerDiagnostic.cmake)
include(cmake/OriginalFrontendMiiResources.cmake)
include(cmake/OriginalFrontendFinalizeDiagnostic.cmake)
include(cmake/OriginalFrontendMenuEntryDiagnostic.cmake)
include(cmake/OriginalFrontendFixedInputDiagnostic.cmake)
include(cmake/OriginalFrontendPlatformPadTaskDiagnostic.cmake)
include(cmake/OriginalFrontendMainMenuDiagnostic.cmake)
include(cmake/OriginalFrontendHallOfFameDiagnostic.cmake)
include(cmake/OriginalFrontendOfflineScenesDiagnostic.cmake)
include(cmake/OriginalFrontendDominationDiagnostic.cmake)
include(cmake/OriginalFrontendChallengesDiagnostic.cmake)
include(cmake/OriginalFrontendBriefingsDiagnostic.cmake)
include(cmake/OriginalFrontendCupDiagnostic.cmake)
include(cmake/OriginalFrontendMatchLoadingDiagnostic.cmake)
include(cmake/OriginalFrontendChallengePlayDiagnostic.cmake)
include(cmake/OriginalFrontendFEModelsDiagnostic.cmake)
include(cmake/OriginalFrontendCreditsDiagnostic.cmake)
include(cmake/OriginalFrontendDVDMessagesDiagnostic.cmake)
include(cmake/OriginalFrontendDVDErrorsDiagnostic.cmake)
include(cmake/NativeAXModuleMemory.cmake)
include(cmake/OriginalOSShutdownRecords.cmake)

# PE: tools/rollback_sections.py launches the compiles of the target and of
# the static and object libraries in it (the script's patterns take the
# members of archives too), once each.
function(mscharged_rollback_sections_launcher target)
    get_property(_done TARGET ${target} PROPERTY MSCHARGED_ROLLBACK_SECTIONS SET)
    if(_done)
        return()
    endif()
    set_property(TARGET ${target} PROPERTY MSCHARGED_ROLLBACK_SECTIONS ON)
    foreach(_language IN ITEMS C CXX)
        set(_launcher "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/rollback_sections.py")
        if(CMAKE_${_language}_COMPILER_LAUNCHER)
            list(APPEND _launcher ${CMAKE_${_language}_COMPILER_LAUNCHER})
        endif()
        set_property(TARGET ${target} PROPERTY ${_language}_COMPILER_LAUNCHER "${_launcher}")
    endforeach()
    get_target_property(_libraries ${target} LINK_LIBRARIES)
    get_target_property(_interface ${target} INTERFACE_LINK_LIBRARIES)
    # Object libraries also come in as sources ($<TARGET_OBJECTS:name>).
    get_target_property(_sources ${target} SOURCES)
    set(_objects)
    foreach(_source IN LISTS _sources)
        if(_source MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
            list(APPEND _objects "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    foreach(_library IN LISTS _libraries _interface _objects)
        if(TARGET "${_library}")
            get_target_property(_imported "${_library}" IMPORTED)
            get_target_property(_type "${_library}" TYPE)
            if(NOT _imported AND _type MATCHES "^(STATIC|OBJECT)_LIBRARY$")
                mscharged_rollback_sections_launcher("${_library}")
            endif()
        endif()
    endforeach()
endfunction()

# One game module per game version, from the same prepared sources:
# mscharged_original_frontend_module_us (R4QE01) and _eu (R4QP01_REV2, which
# also runs European revision 1 discs). The host picks one by the disc.
function(mscharged_add_original_frontend_diagnostic target version suffix)
    string(TOUPPER "${suffix}" _suffix)
    get_target_property(_sources mscharged_original_game_module_template SOURCES)
    add_library(${target} MODULE ${_sources})
    # The shipped file: recharged_game_us/_eu beside the program.
    set_target_properties(${target} PROPERTIES OUTPUT_NAME recharged_game_${suffix})
    add_dependencies(${target} verify_prepared)
    mscharged_add_original_shutdown_records(${target})
    foreach(_property IN ITEMS
            COMPILE_FEATURES COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES
            LINK_LIBRARIES LINK_OPTIONS LINK_DEPENDS MSCHARGED_ORIGINAL_EXPORT_MAP)
        get_target_property(_value mscharged_original_game_module_template ${_property})
        if(_value)
            set_property(TARGET ${target} PROPERTY ${_property} "${_value}")
        endif()
    endforeach()
    set_target_properties(${target} PROPERTIES
        PREFIX "" POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON MSCHARGED_GAME_VERSION ${version})
    # The input sources (KPAD, pads, FE input) differ by version as well.
    if(NOT version STREQUAL "R4QE01")
        mscharged_add_original_game_focus(mscharged_original_game_focus_${suffix} ${version})
        get_target_property(_libraries ${target} LINK_LIBRARIES)
        list(TRANSFORM _libraries REPLACE "^mscharged_original_game_focus$"
            "mscharged_original_game_focus_${suffix}")
        set_property(TARGET ${target} PROPERTY LINK_LIBRARIES "${_libraries}")
    endif()
    # [gameplay] tweaks: the hooks of patches/mscharged-decomp-tweaks are
    # compiled into this game module only; every other target that builds the
    # same original sources keeps their original code unchanged. Defined before
    # the selections: the object libraries some of them make for this module
    # (world views, Credits) copy its definitions when they are made.
    target_compile_definitions(${target} PRIVATE MSCHARGED_TWEAKS=1)
    # Flow omissions are scoped to this diagnostic module only.
    set(MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE ON)
    set(MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE ON)
    mscharged_select_original_frontend_sequence(${target})
    mscharged_select_original_frontend_task_cadence(${target})
    mscharged_select_original_frontend_title(${target})
    mscharged_select_original_frontend_cameras(${target})
    mscharged_select_original_frontend_flash(${target})
    mscharged_select_original_frontend_save_owners(${target})
    mscharged_select_original_frontend_hbm(${target})
    mscharged_select_original_frontend_materials(${target})
    mscharged_select_original_sh_menus(${target})
    mscharged_select_original_game_audio_owner(${target})
    mscharged_select_original_frontend_home(${target})
    # The selected original AxManager owns the optional read-only silence span.
    # Reserve it with the existing one-time module storage admission.
    if(MSCHARGED_DIAGNOSTIC_FRONTEND_HBM)
        mscharged_add_native_ax_module_memory(${target} HBM)
    else()
        mscharged_add_native_ax_module_memory(${target})
    endif()
    mscharged_select_original_game_audio_initialize(${target})
    mscharged_select_original_game_audio_task(${target})
    mscharged_select_original_frontend_nis_owner(${target})
    mscharged_select_original_frontend_audio(${target})
    mscharged_select_original_frontend_boot_script(${target})
    mscharged_select_original_frontend_boot_to_fe(${target})
    mscharged_select_original_frontend_clean_boot(${target})
    mscharged_select_original_frontend_world(${target})
    mscharged_select_original_frontend_world_draw(${target})
    mscharged_add_original_frontend_network_owner_diagnostic(${target})
    mscharged_add_original_frontend_presentation_owner_diagnostic(${target})
    mscharged_select_original_frontend_mii_resources(${target})
    mscharged_select_original_frontend_finalize(${target})
    mscharged_select_original_frontend_menu_entry(${target})
    mscharged_add_original_frontend_fixed_input_diagnostic(${target})
    mscharged_add_original_frontend_platform_pad_task_diagnostic(${target})
    mscharged_select_original_frontend_main_menu(${target})
    mscharged_select_original_frontend_hall_of_fame(${target})
    mscharged_select_original_frontend_offline_scenes(${target})
    mscharged_select_original_frontend_domination(${target})
    mscharged_select_original_frontend_reset(${target})
    mscharged_select_original_frontend_dvd_messages(${target})
    mscharged_select_original_frontend_dvd_errors(${target})
    mscharged_select_original_frontend_challenges(${target})
    mscharged_select_original_frontend_briefings(${target})
    mscharged_select_original_frontend_cup(${target})
    mscharged_select_original_frontend_match_loading(${target})
    mscharged_select_original_frontend_challenge_play(${target})
    mscharged_select_original_frontend_fe_models(${target})
    mscharged_select_original_frontend_lan(${target})
    # After the Mii resources, which add OSMutex without checking for it.
    mscharged_select_original_frontend_online(${target})
    # Last selection: copies the module's final settings for the original CreditScene.
    mscharged_select_original_frontend_credits(${target})
    # RECHARGED_DEBUG_LAYERS_OFF: switch the game's own rendering layers off.
    target_sources(${target} PRIVATE src/platform/debug_layer_toggles.cpp)
    # The Discord status (issue #71): what the game is doing, read by the host after a frame.
    target_sources(${target} PRIVATE src/platform/presence_snapshot.cpp)
    # [gameplay] tweaks: the hooks of patches/mscharged-decomp-tweaks (their
    # MSCHARGED_TWEAKS definition is set above, before the selections).
    target_sources(${target} PRIVATE src/platform/game_tweaks.cpp src/platform/gx_texture.cpp src/platform/away_kits.cpp)
    # The original game's globals in sections of their own (rbk_data/rbk_bss),
    # which a rollback saves with the arenas: a linker script on ELF (GNU ld,
    # lld), and on PE a launcher that renames the same files' sections after
    # each compile (the linker gathers them by name).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_rollback_sections "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rollback_sections.ld")
        target_link_options(${target} PRIVATE "LINKER:-T,${_rollback_sections}")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_rollback_sections}")
    elseif(WIN32)
        mscharged_rollback_sections_launcher(${target})
    endif()
    # The rollback: RECHARGED_ROLLBACK (network matches predict) and RECHARGED_ROLLBACK_TEST
    # (roll matches back while they are played), tweak patches 0029-0031.
    target_sources(${target} PRIVATE src/platform/rollback_snapshot.cpp src/platform/rollback_engine.cpp src/platform/rollback_memory.cpp
        src/platform/rollback_netplay.cpp src/platform/rollback_compare.cpp src/platform/rollback_audio.cpp
        src/platform/rollback_summary.cpp src/platform/rollback_platform.cpp)
    # Robot goalies: the disc's unused robot goalie texture on Charged's goalie (tweak patch 0025).
    target_sources(${target} PRIVATE src/platform/robot_goalie.cpp src/platform/robot_goalie_hook.cpp)
    # Super Team: robot captains and sidekicks from the GameCube robot (tweak patch 0026).
    target_sources(${target} PRIVATE src/platform/super_team.cpp src/platform/super_team_hook.cpp)
    # The logo reads RECHARGED (issue #63, tweak patch 0022, always on).
    target_sources(${target} PRIVATE src/platform/recharged_logo.cpp src/platform/recharged_logo_hook.cpp)
    # The release this module's decomp sources resolve to (checked by the host).
    target_sources(${target} PRIVATE src/platform/game_version.cpp)
    # Graphics preparation happens only before the game starts (all known
    # pipelines, runtime/pipeline_warmup.cpp). The resource-driven variant
    # (MSCHARGED_AUTOMATIC_PREWARM, src/platform/automatic_pipeline_*.cpp,
    # decomp patches 0705-0713) stays off: its loading gates stopped menus and
    # matches until compilation finished, and holding frontend models back left
    # their impostors unset while scene scripts animated them (a crash).
    # dynamic_lookup defers unfinished methods, but missing game vtable/VTT
    # data must be detected before the owner tries to load this module.
    set(_vtable_check "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_original_module_vtables.py")
    if(APPLE)
        set(_vtable_format macho)
    else()
        set(_vtable_format elf)
    endif()
    if(NOT WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" -B "${_vtable_check}"
            --nm "${CMAKE_NM}" --object-format "${_vtable_format}"
            --module "$<TARGET_FILE:${target}>"
        COMMENT "Checking original frontend vtable/VTT imports"
        VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY
        LINK_DEPENDS "${_vtable_check}")
    endif()
    if(WIN32)
        mscharged_import_original_windows_host(${target})
    else()
        add_dependencies(charged_original_game_host ${target})
    endif()
    target_link_libraries(charged_original_game_host PRIVATE mscharged_build_info)
    target_compile_definitions(charged_original_game_host PRIVATE
        MSCHARGED_ORIGINAL_FRONTEND_MODULE_${_suffix}_FILENAME="$<TARGET_FILE_NAME:${target}>")
    if(TARGET mscharged)
        if(NOT WIN32)
            add_dependencies(mscharged ${target})
        endif()
        target_compile_definitions(mscharged PRIVATE MSCHARGED_HAS_ORIGINAL_FRONTEND=1)
    endif()
endfunction()

mscharged_add_original_frontend_diagnostic(mscharged_original_frontend_module_us R4QE01 us)
mscharged_add_original_frontend_diagnostic(mscharged_original_frontend_module_eu R4QP01_REV2 eu)

if(BUILD_TESTING)
    add_test(NAME original_module_vtables
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_module_vtables.py")
endif()
