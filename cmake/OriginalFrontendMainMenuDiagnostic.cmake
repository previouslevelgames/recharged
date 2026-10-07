include_guard(GLOBAL)

# Temporary factory admission. The authored presentation script requests the
# original MainMenu; its save, image, slide and input waits remain source-owned.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU
    "Admit the original MainMenu factory after the authored Title transition" OFF)

function(mscharged_select_original_frontend_main_menu target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_TITLE
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS
            MSCHARGED_DIAGNOSTIC_FRONTEND_FLASH
            MSCHARGED_DIAGNOSTIC_FRONTEND_SAVE_OWNERS
            MSCHARGED_DIAGNOSTIC_FRONTEND_HBM
            MSCHARGED_DIAGNOSTIC_FRONTEND_MII_RESOURCES
            MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER
            MSCHARGED_DIAGNOSTIC_MAIN_PRESENTATION_OWNER
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK)
        if(NOT ${_required})
            message(FATAL_ERROR "Original MainMenu admission requires ${_required}")
        endif()
    endforeach()
    get_target_property(_main_menu_type "${target}" TYPE)
    if(NOT _main_menu_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original MainMenu belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU=1)
    get_target_property(_main_menu_existing "${target}" SOURCES)
    get_target_property(_main_menu_source_dir "${target}" SOURCE_DIR)
    set(_main_menu_absolute)
    foreach(_source IN LISTS _main_menu_existing)
        get_filename_component(_absolute "${_source}" ABSOLUTE
            BASE_DIR "${_main_menu_source_dir}")
        list(APPEND _main_menu_absolute "${_absolute}")
    endforeach()
    foreach(_main_menu_relative IN ITEMS
            src/Game/SH/SHMainMenu.cpp
            src/Game/SH/SHHallOfFame.cpp
            src/Game/FE/feAsyncImage.cpp)
        set(_main_menu_source "${MSCHARGED_PREPARED}/${_main_menu_relative}")
        if(NOT _main_menu_source IN_LIST _main_menu_absolute)
            target_sources("${target}" PRIVATE "${_main_menu_source}")
            list(APPEND _main_menu_absolute "${_main_menu_source}")
        endif()
    endforeach()
    # The original save path uses the existing sole Aurora OS calendar provider.
    # Require a real definition in every executable borrowing this host object.
    if(NOT TARGET charged_original_main_credits_host)
        message(FATAL_ERROR "Original MainMenu requires its existing native host")
    endif()
    target_link_options(charged_original_main_credits_host INTERFACE
        "-Wl,--require-defined=OSTicksToCalendarTime,--export-dynamic-symbol=OSTicksToCalendarTime")
endfunction()
