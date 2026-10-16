include_guard(GLOBAL)

# A separate original-source module leaves the Credits diagnostic intact.
# Both use the same host, with only one module selected for a given run.
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

# One game module per game version, from the same prepared sources:
# mscharged_original_frontend_module_us (R4QE01) and _eu (R4QP01_REV2, which
# also runs European revision 1 discs). The host picks one by the disc.
function(mscharged_add_original_frontend_diagnostic target version suffix)
    string(TOUPPER "${suffix}" _suffix)
    get_target_property(_sources mscharged_original_main_credits_module SOURCES)
    add_library(${target} MODULE ${_sources})
    add_dependencies(${target} verify_prepared)
    mscharged_add_original_shutdown_records(${target})
    foreach(_property IN ITEMS
            COMPILE_FEATURES COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES
            LINK_LIBRARIES LINK_OPTIONS LINK_DEPENDS MSCHARGED_ORIGINAL_EXPORT_MAP)
        get_target_property(_value mscharged_original_main_credits_module ${_property})
        if(_value)
            set_property(TARGET ${target} PROPERTY ${_property} "${_value}")
        endif()
    endforeach()
    set_target_properties(${target} PROPERTIES
        PREFIX "" POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON MSCHARGED_GAME_VERSION ${version})
    # The input sources (KPAD, pads, FE input) differ by version as well.
    if(NOT version STREQUAL "R4QE01")
        mscharged_add_original_main_credits_focus(mscharged_original_main_credits_focus_${suffix} ${version})
        get_target_property(_libraries ${target} LINK_LIBRARIES)
        list(TRANSFORM _libraries REPLACE "^mscharged_original_main_credits_focus$"
            "mscharged_original_main_credits_focus_${suffix}")
        set_property(TARGET ${target} PROPERTY LINK_LIBRARIES "${_libraries}")
    endif()
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
    # Last selection: copies the module's final settings for the original CreditScene.
    mscharged_select_original_frontend_credits(${target})
    # MSCHARGED_DEBUG_LAYERS_OFF: switch the game's own rendering layers off.
    target_sources(${target} PRIVATE src/platform/debug_layer_toggles.cpp)
    # [gameplay] tweaks: the hooks of patches/mscharged-decomp-tweaks are
    # compiled into this game module only; every other target that builds the
    # same original sources keeps their original code unchanged.
    target_compile_definitions(${target} PRIVATE MSCHARGED_TWEAKS=1)
    target_sources(${target} PRIVATE src/platform/game_tweaks.cpp src/platform/away_kits.cpp)
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
        add_dependencies(charged_original_main_credits_host ${target})
    endif()
    target_link_libraries(charged_original_main_credits_host PRIVATE mscharged_build_info)
    target_compile_definitions(charged_original_main_credits_host PRIVATE
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
