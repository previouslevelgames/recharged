include_guard(GLOBAL)
include(cmake/OriginalHBMTaskLifecycle.cmake)

# Attach the capacity fixture to the existing sole whole-source task/MEM module.
# This adds no HBM main call, archive owner or worker to production execution.
function(mscharged_add_original_hbm_task_capacity_tests)
    if(NOT BUILD_TESTING)
        return()
    endif()
    mscharged_add_original_hbm_task_lifecycle_tests()
    if(NOT TARGET mscharged_original_hbm_task_module OR TARGET original_hbm_task_capacity_tests)
        return()
    endif()
    target_sources(mscharged_original_hbm_task_module PRIVATE
        tests/diagnostics/original_hbm_task_capacity.cpp)
    set_property(SOURCE tests/diagnostics/original_hbm_task_capacity.cpp
        TARGET_DIRECTORY mscharged_original_hbm_task_module
        APPEND PROPERTY COMPILE_OPTIONS -fno-access-control)
    mscharged_add_original_hbm_task_test_exports(mscharged_original_hbm_task_module
        charged_hbm_task_capacity charged_hbm_task_storage_owner charged_hbm_task_storage_bytes)

    add_executable(original_hbm_task_capacity_tests
        tests/original_hbm_task_capacity.cpp src/platform/host_metadata.cpp)
    add_dependencies(original_hbm_task_capacity_tests mscharged_original_hbm_task_module)
    target_compile_features(original_hbm_task_capacity_tests PRIVATE cxx_std_20)
    mscharged_link_original_hbm_debug_host(original_hbm_task_capacity_tests CPU_FIXTURE)
    target_compile_definitions(original_hbm_task_capacity_tests PRIVATE HBM_ASSERT=1)
    target_link_libraries(original_hbm_task_capacity_tests PRIVATE
        charged_original_os_messages "${CMAKE_DL_LIBS}")
    if(APPLE)
        target_link_options(original_hbm_task_capacity_tests PRIVATE "LINKER:-export_dynamic")
    else()
        target_link_options(original_hbm_task_capacity_tests PRIVATE "LINKER:--export-dynamic")
    endif()
    foreach(_symbol IN ITEMS OSInitMessageQueue OSSendMessage OSReceiveMessage ChargedNativeMetadataRelease)
        mscharged_require_original_host_symbol(original_hbm_task_capacity_tests PRIVATE "${_symbol}")
    endforeach()
    add_test(NAME original_hbm_task_capacity COMMAND original_hbm_task_capacity_tests
        "$<TARGET_FILE:mscharged_original_hbm_task_module>")
    set_tests_properties(original_hbm_task_capacity PROPERTIES TIMEOUT 15 LABELS "Platform")
endfunction()
