include_guard(GLOBAL)
include(cmake/ParticleSimulation.cmake)
include(cmake/ParticleControllers.cmake)
include(cmake/ColourMesh.cmake)
add_library(charged_particle_render STATIC src/runtime/particle_render.cpp src/runtime/particle_controller_render.cpp)
add_dependencies(charged_particle_render verify_prepared)
target_link_libraries(charged_particle_render PUBLIC charged_particle_controllers charged_colour_mesh charged_frames)
target_compile_features(charged_particle_render PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(charged_particle_render PRIVATE -ffp-contract=off)
endif()
if(BUILD_TESTING)
    add_executable(particle_render_tests tests/particle_render.cpp)
    target_link_libraries(particle_render_tests PRIVATE charged_particle_render)
    add_test(NAME particle_render COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_particle_render.py" "$<TARGET_FILE:particle_render_tests>")
    set_tests_properties(particle_render PROPERTIES TIMEOUT 30)
    if(MSCHARGED_TEST_VULKAN)
        add_executable(particle_pipeline_tests tests/particle_pipeline.cpp)
        target_link_libraries(particle_pipeline_tests PRIVATE charged_particle_render charged_static_inventory aurora::gx aurora::vi aurora::core)
        add_test(NAME particle_pipeline COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_particle_render.py" "$<TARGET_FILE:particle_pipeline_tests>")
        set_tests_properties(particle_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
            ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
            FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
        add_executable(particle_controller_pipeline_tests tests/particle_controller_pipeline.cpp)
        target_link_libraries(particle_controller_pipeline_tests PRIVATE charged_particle_render charged_static_inventory aurora::gx aurora::vi aurora::core)
        add_test(NAME particle_controller_pipeline COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_particle_controller.py" "$<TARGET_FILE:particle_controller_pipeline_tests>")
        set_tests_properties(particle_controller_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
            ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
            FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    endif()
endif()
