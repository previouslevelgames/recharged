include_guard(GLOBAL)

# Temporary admission of the original Cup room requested by the authored
# MainMenu transition. Save/preload/camera/slide/input decisions stay in source.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_HALL_OF_FAME
    "Admit the original Hall of Fame Cup rooms after the authored MainMenu transition" OFF)

function(mscharged_select_original_frontend_hall_of_fame target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_HALL_OF_FAME)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_MAIN_MENU
            MSCHARGED_DIAGNOSTIC_FRONTEND_CAMERAS)
        if(NOT ${_required})
            message(FATAL_ERROR "Original Hall of Fame admission requires ${_required}")
        endif()
    endforeach()
    get_target_property(_hof_type "${target}" TYPE)
    if(NOT _hof_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original Hall of Fame belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_HALL_OF_FAME=1)
    get_target_property(_hof_existing "${target}" SOURCES)
    get_target_property(_hof_source_dir "${target}" SOURCE_DIR)
    set(_hof_absolute)
    foreach(_source IN LISTS _hof_existing)
        get_filename_component(_absolute "${_source}" ABSOLUTE
            BASE_DIR "${_hof_source_dir}")
        list(APPEND _hof_absolute "${_absolute}")
    endforeach()
    set(_hof_source "${MSCHARGED_PREPARED}/src/Game/SH/SHHallOfFameRoom.cpp")
    if(NOT _hof_source IN_LIST _hof_absolute)
        target_sources("${target}" PRIVATE "${_hof_source}")
    endif()
endfunction()
