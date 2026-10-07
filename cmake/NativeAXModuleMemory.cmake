include_guard(GLOBAL)
include(cmake/NativeDSPMemory.cmake)
include(cmake/NativeAI.cmake)

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
    charged_native_dsp_memory charged_native_ai PRIVATE aurora::os ${CMAKE_DL_LIBS})

function(mscharged_add_native_ax_module_memory target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Actual AX module-memory admission currently requires Linux LP64")
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
    target_sources("${target}" PRIVATE src/platform/native_ax_module_storage.cpp)
    target_compile_definitions("${target}" PRIVATE MSCHARGED_NATIVE_AX_MODULE_MEMORY=1)
    target_compile_features("${target}" PRIVATE c_std_17)
    # Existing host is shared by the selected Credits/frontend modules; only
    # the frontend loader arms this endpoint. Credits retains its own module.
    target_link_libraries(charged_original_main_credits_host PRIVATE
        charged_native_ax_module_memory)
endfunction()
