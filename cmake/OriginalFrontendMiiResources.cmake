include_guard(GLOBAL)
include(cmake/OriginalARC.cmake)
include(cmake/OriginalRFLResource.cmake)
include(cmake/OriginalRFLCharacterSources.cmake)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_MII_RESOURCES
    "Admit original frontend Mii resource loading" OFF)

function(mscharged_select_original_frontend_mii_resources target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_MII_RESOURCES)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD)
        message(FATAL_ERROR "Original Mii resource loading requires the original frontend world path")
    endif()
    mscharged_add_original_arc("${target}")
    mscharged_add_original_rfl_resource("${target}")
    mscharged_add_original_rfl_characters("${target}")
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_MII_RESOURCES=1)
    target_sources("${target}" PRIVATE
        "${MSCHARGED_PREPARED}/src/Game/MiiManager.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSMutex.c")
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSMutex.c"
        TARGET_DIRECTORY "${target}" APPEND PROPERTY COMPILE_OPTIONS
        -fexceptions -Werror=pointer-to-int-cast -Werror=implicit-function-declaration)
    # The original constructor and NL callback initialize resources. CreateIcon
    # remains outside this selected runtime path; no pixel data is synthesized.
    foreach(_leaf IN ITEMS RFL_HiddenDatabase RFL_MiddleDatabase RFL_Controller
            RFL_NANDAccess RFL_Format)
        set(_path "${MSCHARGED_PREPARED}/src/RVL_SDK/rfl/${_leaf}.c")
        target_sources("${target}" PRIVATE "${_path}")
        set_property(SOURCE "${_path}" TARGET_DIRECTORY "${target}" APPEND
            PROPERTY COMPILE_OPTIONS -fexceptions -Werror=pointer-to-int-cast
            -Werror=implicit-function-declaration)
    endforeach()
endfunction()
