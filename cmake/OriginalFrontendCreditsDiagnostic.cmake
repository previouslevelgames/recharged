include_guard(GLOBAL)

# Temporary admission of the original CreditScene requested by OptionsScene.
# Credits loading, movie, text, music and return decisions stay in source.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_CREDITS
    "Admit the original CreditScene selected from the original Options menu" OFF)

function(mscharged_select_original_frontend_credits target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_CREDITS)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_SH_MENUS)
        if(NOT ${_required})
            message(FATAL_ERROR "Original Credits admission requires ${_required}")
        endif()
    endforeach()
    get_target_property(_credits_type "${target}" TYPE)
    if(NOT _credits_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original Credits belong to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_CREDITS=1)
    # This module also carries the standalone Credits test definitions. Compile
    # the original CreditScene without them, with the module's final settings,
    # so its genuine NINTENDO/NLG/credits/copyright/return phases all run.
    # No header depends on these definitions; the class layout is identical.
    set(_credits_source "${MSCHARGED_PREPARED}/src/Game/SH/SHCredits.cpp")
    get_target_property(_credits_sources "${target}" SOURCES)
    get_target_property(_credits_source_dir "${target}" SOURCE_DIR)
    set(_credits_kept)
    foreach(_source IN LISTS _credits_sources)
        get_filename_component(_absolute "${_source}" ABSOLUTE BASE_DIR "${_credits_source_dir}")
        if(NOT _absolute STREQUAL _credits_source)
            list(APPEND _credits_kept "${_source}")
        endif()
    endforeach()
    # Replace the inherited standalone-test compilation of the same scene.
    set_property(TARGET "${target}" PROPERTY SOURCES "${_credits_kept}")
    set(_credits_objects mscharged_original_frontend_credits)
    add_library("${_credits_objects}" OBJECT "${_credits_source}")
    add_dependencies("${_credits_objects}" verify_prepared)
    set_target_properties("${_credits_objects}" PROPERTIES POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    foreach(_property IN ITEMS COMPILE_FEATURES COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_LIBRARIES)
        get_target_property(_value "${target}" "${_property}")
        if(_value)
            set_property(TARGET "${_credits_objects}" PROPERTY "${_property}" "${_value}")
        endif()
    endforeach()
    get_target_property(_definitions "${target}" COMPILE_DEFINITIONS)
    list(REMOVE_ITEM _definitions MSCHARGED_DIAGNOSTIC_CREDITS_SCENE=1
        MSCHARGED_DIAGNOSTIC_CREDITS_MOVIE=1 MSCHARGED_DIAGNOSTIC_CREDITS_COPYRIGHTS=1)
    set_property(TARGET "${_credits_objects}" PROPERTY COMPILE_DEFINITIONS "${_definitions}")
    target_sources("${target}" PRIVATE "$<TARGET_OBJECTS:${_credits_objects}>")
endfunction()
