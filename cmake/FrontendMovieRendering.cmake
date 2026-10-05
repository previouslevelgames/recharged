include_guard(GLOBAL)
# Include after the normal material registry has selected original Movie TUs
# and the callback-free movie_draw_observer provider (parent ScenePreview).
add_library(charged_frontend_movie_render STATIC src/runtime/frontend_movie_render.cpp
    "${MSCHARGED_PREPARED}/src/Game/GL/GLMovieMeshWriter.cpp")
add_dependencies(charged_frontend_movie_render verify_prepared)
target_compile_features(charged_frontend_movie_render PUBLIC cxx_std_20)
target_include_directories(charged_frontend_movie_render PUBLIC src "${MSCHARGED_PREPARED}/include"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_frontend_movie_render PRIVATE MSCHARGED_NATIVE=1)
target_compile_options(charged_frontend_movie_render PRIVATE -ffp-contract=off -fno-strict-aliasing)
target_link_libraries(charged_frontend_movie_render PUBLIC charged_frontend_movie_playback charged_frames charged_shadows charged_materials)
if(BUILD_TESTING)
    add_executable(frontend_movie_pipeline_tests tests/frontend_movie_pipeline.cpp)
    target_link_libraries(frontend_movie_pipeline_tests PRIVATE charged_frontend_movie_render aurora::dvd)
endif()
