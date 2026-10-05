include_guard(GLOBAL)
include(cmake/FrontendOptions.cmake)
include(cmake/FrontendNavigation.cmake)
add_library(charged_frontend_options_navigation STATIC src/runtime/frontend_options_navigation.cpp)
add_dependencies(charged_frontend_options_navigation verify_prepared)
target_link_libraries(charged_frontend_options_navigation PUBLIC charged_frontend_options charged_frontend_navigation)
target_compile_features(charged_frontend_options_navigation PUBLIC cxx_std_20)
if(BUILD_TESTING)
    include(cmake/FrontendStack.cmake)
    add_executable(frontend_options_navigation_tests tests/frontend_options_navigation.cpp)
    target_link_libraries(frontend_options_navigation_tests PRIVATE charged_frontend_options_navigation
        charged_frontend_stack charged_audio_bank_load aurora::dvd aurora::core)
    if(NOT MSCHARGED_BUILD_SCENE_PREVIEW)
        add_test(NAME frontend_options_navigation COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend_options_navigation.py" "$<TARGET_FILE:frontend_options_navigation_tests>")
        set_tests_properties(frontend_options_navigation PROPERTIES TIMEOUT 120
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
    endif()
endif()
