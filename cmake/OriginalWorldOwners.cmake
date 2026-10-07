include_guard(GLOBAL)

function(_mscharged_world_owned_sources target output)
    if("${target}" IN_LIST ARGN)
        set("${output}" "" PARENT_SCOPE)
        return()
    endif()
    set(_visited ${ARGN} "${target}")
    get_target_property(_sources "${target}" SOURCES)
    set(_all ${_sources})
    foreach(_source IN LISTS _sources)
        if(_source MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
            _mscharged_world_owned_sources("${CMAKE_MATCH_1}" _nested ${_visited})
            list(APPEND _all ${_nested})
        endif()
    endforeach()
    # Object libraries can also enter the module through target_link_libraries,
    # as the existing input cohort does for the original TweakConfig provider.
    get_target_property(_links "${target}" LINK_LIBRARIES)
    foreach(_link IN LISTS _links)
        if(TARGET "${_link}")
            get_target_property(_kind "${_link}" TYPE)
            if(_kind STREQUAL "OBJECT_LIBRARY")
                _mscharged_world_owned_sources("${_link}" _nested ${_visited})
                list(APPEND _all ${_nested})
            endif()
        endif()
    endforeach()
    set("${output}" "${_all}" PARENT_SCOPE)
endfunction()

# Original source providers and raw/native placement storage. This helper adds
# no loading-script admission, readiness, camera state or rendering outcome.
# Its caller must retain original pool/camera/file predecessors and lifecycle.
function(mscharged_add_original_world_owners target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY" OR
            NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
            NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
            NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Original world owners require the qualified isolated Linux LP64 module profile")
    endif()
    get_target_property(_existing "${target}" SOURCES)
    _mscharged_world_owned_sources("${target}" _known)
    foreach(_source IN ITEMS
            src/Game/world.cpp
            src/Game/BasicStadium.cpp
            src/Game/Render/StadiumLoading.cpp
            src/Game/Render/StadiumWorldObjects.cpp
            src/Game/Render/tu_8027AE14.cpp
            src/Game/Render/StadiumPhysicsObject.cpp
            src/Game/World/worldanim.cpp
            src/Game/World/worldanimobjects.cpp
            src/Game/World/WorldEffect.cpp
            src/Game/World/WorldPhysics.cpp
            src/Game/World/WorldVisibility.cpp
            src/Game/Render/CrowdLayoutObject.cpp
            src/Game/Render/WorldNPC.cpp
            src/Game/FE/feModelManager.cpp
            src/Game/SAnim/pnSAnimController.cpp
            src/Game/SAnimDecode.cpp
            src/Game/Render/Warble.cpp
            src/Game/GameObjectLighting.cpp
            src/Game/PoseNode.cpp
            src/Game/TweakFileLoader.cpp
            src/Game/TweakConfig.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _known)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _existing "${_path}")
        endif()
    endforeach()
    foreach(_path IN ITEMS
            src/platform/world_record_storage.cpp
            src/platform/world_animation_data_transport.cpp)
        if(NOT _path IN_LIST _known)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _existing "${_path}")
        endif()
    endforeach()

    # These four original owners supply the actual source world views. Retire
    # their inherited Credits-only source exclusion in this module alone;
    # parent definitions and the separate Credits module remain unchanged.
    set(_view_sources)
    foreach(_source IN ITEMS
            src/Game/Render/RLViewLayers.cpp
            src/Game/Render/HighRange.cpp
            src/Game/Render/ShadowVolume.cpp
            src/Game/Render/RenderShadow.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(_path IN_LIST _known AND NOT _path IN_LIST _existing)
            message(FATAL_ERROR "A world view provider already belongs to a nested object owner; select its full original profile there")
        endif()
        list(REMOVE_ITEM _existing "${_path}")
        list(APPEND _view_sources "${_path}")
    endforeach()
    set_property(TARGET "${target}" PROPERTY SOURCES "${_existing}")
    set(_views "${target}_world_views")
    add_library("${_views}" OBJECT ${_view_sources})
    add_dependencies("${_views}" verify_prepared)
    set_target_properties("${_views}" PROPERTIES POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    foreach(_property IN ITEMS COMPILE_FEATURES COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_LIBRARIES)
        get_target_property(_value "${target}" "${_property}")
        if(_value)
            set_property(TARGET "${_views}" PROPERTY "${_property}" "${_value}")
        endif()
    endforeach()
    get_target_property(_definitions "${target}" COMPILE_DEFINITIONS)
    list(REMOVE_ITEM _definitions MSCHARGED_DIAGNOSTIC_CREDITS_SCENE=1)
    set_property(TARGET "${_views}" PROPERTY COMPILE_DEFINITIONS "${_definitions}")
    target_sources("${target}" PRIVATE "$<TARGET_OBJECTS:${_views}>")
endfunction()
