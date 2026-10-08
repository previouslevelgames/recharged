include_guard(GLOBAL)
include(cmake/NativeDSPMemory.cmake)
include(cmake/NativeAI.cmake)
include(cmake/NativeModuleLoader.cmake)

# One host owns the actual image leases/device spans; the original module links
# no SDK or host allocation provider. This is memory admission, not AX readiness.
add_library(charged_native_ax_module_memory STATIC
    src/platform/native_ax_module_memory.cpp)
add_dependencies(charged_native_ax_module_memory verify_prepared)
target_include_directories(charged_native_ax_module_memory PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_ax_module_memory PUBLIC TARGET_PC=1)
target_compile_features(charged_native_ax_module_memory PUBLIC cxx_std_17)
target_link_libraries(charged_native_ax_module_memory PUBLIC
    charged_native_dsp_memory charged_native_ai charged_native_module_loader PRIVATE aurora::os)

function(mscharged_add_native_ax_module_memory target)
    cmake_parse_arguments(_storage "HBM" "" "" ${ARGN})
    if(_storage_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown original AX storage arguments: ${_storage_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Actual AX module-memory admission requires Linux or macOS LP64")
    endif()
    get_target_property(_kind "${target}" TYPE)
    if(NOT _kind STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "AX source statics require the isolated original game module")
    endif()
    get_target_property(_existing "${target}" SOURCES)
    foreach(_name IN ITEMS AX AXAlloc AXAux AXCL AXComp AXSPB AXVPB AXProf AXOut DSPCode)
        set(_path "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/${_name}.c")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
        endif()
    endforeach()
    if(_storage_HBM)
        set(_hbm_owner "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/snd_AxManager.cpp")
        if(NOT _hbm_owner IN_LIST _existing)
            message(FATAL_ERROR "HBM static storage requires the actual whole AxManager owner in this module")
        endif()
        target_compile_definitions("${target}" PRIVATE MSCHARGED_NATIVE_HBM_MODULE_MEMORY=1)
    endif()
    if(APPLE)
        # Mach-O does not sort constructor priorities across translation units.
        # A first OBJECT provider also precedes existing inflater/view/focus
        # objects, which CMake places ahead of ordinary module source objects.
        set(_early "${target}_ax_pre_static")
        add_library("${_early}" OBJECT src/platform/native_ax_module_storage.cpp)
        add_dependencies("${_early}" verify_prepared)
        foreach(_property IN ITEMS
                COMPILE_FEATURES COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES)
            get_target_property(_value "${target}" "${_property}")
            if(_value)
                set_property(TARGET "${_early}" PROPERTY "${_property}" "${_value}")
            endif()
        endforeach()
        set_target_properties("${_early}" PROPERTIES
            POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
        target_link_libraries("${_early}" PRIVATE charged_original_function_pool_abi)
        target_compile_definitions("${_early}" PRIVATE MSCHARGED_NATIVE_AX_MODULE_MEMORY=1)
        get_target_property(_source_order "${target}" SOURCES)
        set_property(TARGET "${target}" PROPERTY SOURCES
            "$<TARGET_OBJECTS:${_early}>" ${_source_order})
    else()
        target_sources("${target}" PRIVATE src/platform/native_ax_module_storage.cpp)
    endif()
    target_compile_definitions("${target}" PRIVATE MSCHARGED_NATIVE_AX_MODULE_MEMORY=1)
    target_compile_features("${target}" PRIVATE c_std_17)
    # Existing host is shared by the selected Credits/frontend modules; only
    # the frontend loader arms this endpoint. Credits retains its own module.
    target_link_libraries(charged_original_main_credits_host PRIVATE
        charged_native_ax_module_memory)
endfunction()
