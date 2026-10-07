include_guard(GLOBAL)
include(cmake/OriginalMaterialSources.cmake)

# Restore all source registry entries in this selected frontend module. Original
# glStartup and main own construction and Initialize traversal. Real resource,
# lighting, actor and camera owners remain prerequisites for each material draw.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_MATERIALS
    "Admit all original material programs in the frontend diagnostic" OFF)
function(mscharged_select_original_frontend_materials target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_MATERIALS)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE)
        message(FATAL_ERROR "Material admission requires the original frontend sequence diagnostic")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original material providers belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_MATERIALS=1)
    get_target_property(_existing "${target}" SOURCES)
    mscharged_original_material_sources(_materials)
    foreach(_source IN LISTS _materials)
        if(NOT _source IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_source}")
        endif()
    endforeach()
endfunction()
