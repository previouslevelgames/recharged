include_guard(GLOBAL)

# Admit only the original AsyncLoading HBM resource services26/27. The real
# source owns construction, language-selected requests, six NL completions and
# finalization. This does not initialize/display the HBM library or admit menus.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_HBM
    "Admit original HBM resource services in the selected frontend diagnostic" OFF)
function(mscharged_select_original_frontend_hbm target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_HBM)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE)
        message(FATAL_ERROR "HBM resource admission requires the named original frontend sequence")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HBM providers belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_HBM=1)
    get_target_property(_existing "${target}" SOURCES)
    set(_hbm_source "${MSCHARGED_PREPARED}/src/Game/HBMManager.cpp")
    if(NOT _hbm_source IN_LIST _existing)
        target_sources("${target}" PRIVATE "${_hbm_source}")
    endif()
    # The sequence's original SaveLoad/Wii16/TPL transport already supplies the
    # actual qualified palette binder. HBMManager source remains unmodified.
endfunction()
