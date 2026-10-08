include_guard(GLOBAL)
include(cmake/OriginalHBMAnimationTransport.cmake)
include(cmake/OriginalHBMArcResources.cmake)
include(cmake/OriginalHBMFontTransport.cmake)
include(cmake/OriginalMEMExpHeap.cmake)
include(cmake/OriginalTPL.cmake)

# Whole original software providers only. Neither this selector nor its static
# constructors install Layout::mspAllocator or create a HOME owner. The caller
# must also compose the reviewed BRLYT data bridge before executing Build.
function(mscharged_select_original_hbm_layout_providers target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Original HOME layout needs its actual source module")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HOME layout must share the sole game module")
    endif()
    mscharged_select_original_hbm_animation_transport("${target}")
    mscharged_select_original_hbm_arc_resources("${target}")
    mscharged_select_original_hbm_font_transport("${target}")
    mscharged_select_original_hbm_debug("${target}")
    mscharged_add_original_tpl("${target}")
    mscharged_add_original_mem_exp_heap("${target}")
    target_link_libraries("${target}" PRIVATE charged_wii_msl)
    get_target_property(_selected "${target}" SOURCES)
    foreach(_relative IN ITEMS
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_pane.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_group.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_window.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_picture.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_textBox.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_bounding.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_material.cpp
        src/RVL_SDK/hbm/nw4hbm/lyt/lyt_drawInfo.cpp
        src/RVL_SDK/hbm/nw4hbm/math/math_triangular.cpp
        src/RVL_SDK/hbm/nw4hbm/ut/ut_CharWriter.cpp
        src/RVL_SDK/hbm/nw4hbm/ut/ut_TextWriterBase.cpp
        src/RVL_SDK/hbm/nw4hbm/ut/ut_TagProcessorBase.cpp
        src/RVL_SDK/hbm/nw4hbm/ut/ut_list.cpp
        src/RVL_SDK/hbm/HBMAnmController.cpp
        src/RVL_SDK/hbm/HBMFrameController.cpp
        src/RVL_SDK/hbm/HBMRemoteSpk.cpp
        src/RVL_SDK/hbm/HBMController.cpp
        src/RVL_SDK/mem/mem_allocator.c)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _selected)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _selected "${_source}")
        endif()
    endforeach()
    # The original generic allocator remains C and retains its Exp/Frm choices.
    # Unused Frm initializer code can be stripped; this admits no Frm owner.
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_allocator.c"
        TARGET_DIRECTORY "${target}" PROPERTY LANGUAGE C)
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/RVL_SDK/mem/mem_allocator.c"
        TARGET_DIRECTORY "${target}" APPEND PROPERTY COMPILE_OPTIONS
        -fexceptions -Werror=pointer-to-int-cast -Werror=implicit-function-declaration)
endfunction()
