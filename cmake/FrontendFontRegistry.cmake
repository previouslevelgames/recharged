include_guard(GLOBAL)
include(cmake/ColourMesh.cmake)
add_library(charged_frontend_font_registry STATIC
    src/runtime/frontend_font_registry.cpp src/runtime/frontend_font_packets.cpp
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw2.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glQuadSupport.cpp")
add_dependencies(charged_frontend_font_registry verify_prepared)
target_link_libraries(charged_frontend_font_registry PUBLIC charged_frontend_fonts charged_colour_mesh charged_frames)
target_include_directories(charged_frontend_font_registry PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_features(charged_frontend_font_registry PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(charged_frontend_font_registry PRIVATE -ffp-contract=off)
endif()
if(BUILD_TESTING)
    add_executable(frontend_font_registry_tests tests/frontend_font_registry.cpp)
    target_link_libraries(frontend_font_registry_tests PRIVATE charged_frontend_font_registry)
    add_test(NAME frontend_font_registry COMMAND frontend_font_registry_tests)
    set_tests_properties(frontend_font_registry PROPERTIES TIMEOUT 30)
    if(MSCHARGED_TEST_VULKAN)
        add_executable(frontend_font_pipeline_tests tests/frontend_font_pipeline.cpp)
        target_link_libraries(frontend_font_pipeline_tests PRIVATE charged_frontend_font_registry aurora::gx aurora::vi aurora::core)
        add_test(NAME frontend_font_pipeline COMMAND frontend_font_pipeline_tests)
        set_tests_properties(frontend_font_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
            ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
            FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    endif()
endif()
