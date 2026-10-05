include_guard(GLOBAL)
add_library(charged_frontend_movie_binding STATIC src/runtime/frontend_movie_binding.cpp)
target_compile_features(charged_frontend_movie_binding PUBLIC cxx_std_20)
target_include_directories(charged_frontend_movie_binding PUBLIC src)
target_link_libraries(charged_frontend_movie_binding PUBLIC charged_frontend_movie_playback charged_frontend_session)
# Renderer/test linkage is selected only in ScenePreview, since actual movie
# bindings require real original graphics registration and frame services.
