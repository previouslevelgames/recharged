include_guard(GLOBAL)
include(cmake/FrontendPointerHost.cmake)
add_library(charged_frontend_pointer_display STATIC src/runtime/frontend_pointer_display.cpp)
target_link_libraries(charged_frontend_pointer_display PUBLIC charged_frontend_pointer_host aurora::core)
target_compile_features(charged_frontend_pointer_display PUBLIC cxx_std_20)
if(BUILD_TESTING AND MSCHARGED_TEST_VULKAN)
    add_executable(frontend_pointer_display_pipeline_tests tests/frontend_pointer_display_pipeline.cpp src/runtime/gpu_readback.cpp)
    target_link_libraries(frontend_pointer_display_pipeline_tests PRIVATE charged_frontend_pointer_display charged_frontend_layout_gx aurora::gx aurora::vi)
    add_test(NAME frontend_pointer_display_pipeline COMMAND frontend_pointer_display_pipeline_tests)
    set_tests_properties(frontend_pointer_display_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
endif()
