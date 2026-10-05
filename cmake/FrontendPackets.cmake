include_guard(GLOBAL)
include(cmake/FrontendSession.cmake)
include(cmake/FrontendFontRegistry.cmake)
add_library(charged_frontend_packets STATIC
    src/runtime/frontend_packets.cpp
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw3.cpp")
add_dependencies(charged_frontend_packets verify_prepared)
target_link_libraries(charged_frontend_packets PUBLIC charged_frontend_session charged_frontend_font_registry
    charged_frontend_movie_binding charged_frontend_movie_render)
target_compile_features(charged_frontend_packets PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(charged_frontend_packets PRIVATE -ffp-contract=off)
endif()
if(BUILD_TESTING)
    add_executable(frontend_packets_tests tests/frontend_packets.cpp)
    target_link_libraries(frontend_packets_tests PRIVATE charged_frontend_packets)
    add_test(NAME frontend_packets COMMAND frontend_packets_tests)
    set_tests_properties(frontend_packets PROPERTIES TIMEOUT 60)
    add_executable(frontend_movie_image_tests tests/frontend_movie_image.cpp)
    target_link_libraries(frontend_movie_image_tests PRIVATE charged_frontend_packets aurora::dvd)
    add_test(NAME frontend_movie_image COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend_movie_image.py" "$<TARGET_FILE:frontend_movie_image_tests>")
    set_tests_properties(frontend_movie_image PROPERTIES TIMEOUT 90
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
    if(MSCHARGED_TEST_VULKAN)
        add_executable(frontend_movie_image_pipeline_tests tests/frontend_movie_image_pipeline.cpp)
        target_link_libraries(frontend_movie_image_pipeline_tests PRIVATE charged_frontend_packets aurora::dvd)
        add_test(NAME frontend_movie_image_pipeline COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend_movie_image.py" "$<TARGET_FILE:frontend_movie_image_pipeline_tests>")
        set_tests_properties(frontend_movie_image_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
            ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;SDL_AUDIODRIVER=dummy" RESOURCE_LOCK gx_check
            FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
        add_executable(frontend_packet_pipeline_tests tests/frontend_packet_pipeline.cpp)
        target_link_libraries(frontend_packet_pipeline_tests PRIVATE charged_frontend_packets aurora::gx aurora::vi aurora::core)
        add_test(NAME frontend_packet_pipeline COMMAND frontend_packet_pipeline_tests)
        set_tests_properties(frontend_packet_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
            ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
            FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    endif()
endif()
