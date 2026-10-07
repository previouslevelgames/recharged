include_guard(GLOBAL)

# Temporary source-factory integration; normal full factory and Credits remain
# unchanged. This does not complete original Title->MainMenu or its network,
# save/audio/presentation predecessors.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS
    "Admit original Options and Popup factories in the frontend diagnostic" OFF)

function(mscharged_select_original_sh_menus target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE)
        message(FATAL_ERROR "SH menu integration requires the named frontend sequence")
    endif()
    get_target_property(_sh_type "${target}" TYPE)
    if(NOT _sh_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "SH handlers belong to the isolated original source module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS=1)
    target_sources("${target}" PRIVATE tests/diagnostics/original_sh_menu.cpp)
    get_target_property(_sh_link_options "${target}" LINK_OPTIONS)
    list(REMOVE_ITEM _sh_link_options
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_main_credits_exports.map")
    set_property(TARGET "${target}" PROPERTY LINK_OPTIONS "${_sh_link_options}")
    target_link_options("${target}" PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_sh_menu_exports.map")
    set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_sh_menu_exports.map")
    get_target_property(_sh_existing "${target}" SOURCES)
    foreach(_sh_source IN ITEMS
        src/Game/SH/SHOptions.cpp
        src/Game/SH/SHStadiumSelect.cpp
        src/Game/FE/feOptionsSubMenus.cpp
        src/Game/FE/fePopupMenu.cpp
        src/Game/FE/feBackButton.cpp
        src/Game/FE/feMusic.cpp
        src/Game/Task/ResetTask.cpp)
        set(_sh_path "${MSCHARGED_PREPARED}/${_sh_source}")
        if(NOT _sh_path IN_LIST _sh_existing)
            target_sources("${target}" PRIVATE "${_sh_path}")
            list(APPEND _sh_existing "${_sh_path}")
        endif()
    endforeach()
    # The original Options TU retains its six Wii16 L"%d" literals and the
    # existing original MSL wide formatter; do not apply wchar2 to other TUs.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        foreach(_sh_wii16 IN ITEMS
                src/Game/FE/feOptionsSubMenus.cpp src/Game/SH/SHStadiumSelect.cpp)
            set_property(SOURCE "${MSCHARGED_PREPARED}/${_sh_wii16}"
                APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
        endforeach()
    endif()
    if(TARGET mscharged)
        target_compile_definitions(mscharged PRIVATE MSCHARGED_HAS_ORIGINAL_SH_MENUS=1)
    endif()
endfunction()
