include_guard(GLOBAL)

# Retire the selected diagnostic's three original save-owner omissions.
# Original main constructs them in source order; settings/CRC/NAND callbacks,
# menu readiness and normal game startup still require their own integration.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_SAVE_OWNERS
    "Admit original cup/statistics/challenge owners in the frontend diagnostic" OFF)
function(mscharged_select_original_frontend_save_owners target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SAVE_OWNERS)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_FLASH)
        message(FATAL_ERROR "Save-owner admission requires the original frontend/flash diagnostic")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original save owners belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_SAVE_OWNERS=1)
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN ITEMS
        src/Game/DB/CupManager.cpp
        src/Game/DB/StatsTracker.cpp
        src/Game/DB/StrikerChallenge.cpp
        src/Game/DB/Simmer.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
        endif()
    endforeach()
endfunction()
