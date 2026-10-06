include_guard(GLOBAL)
add_library(charged_frontend_input STATIC
    "${MSCHARGED_PREPARED}/src/NL/plat/PadBackend.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/globalpad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/cGlobalPad.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feInput.cpp"
    src/runtime/frontend_input.cpp src/runtime/frontend_input_sdl.cpp)
add_dependencies(charged_frontend_input verify_prepared)
target_compile_definitions(charged_frontend_input PUBLIC MSCHARGED_DIAGNOSTIC_INPUT=1)
target_compile_definitions(charged_frontend_input PRIVATE MSCHARGED_NATIVE=1)
target_compile_features(charged_frontend_input PUBLIC cxx_std_20)
target_link_libraries(charged_frontend_input PUBLIC charged_graphics_memory SDL3::SDL3-static)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_frontend_input PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char)
endif()
if(BUILD_TESTING)
    add_executable(frontend_input_tests tests/frontend_input.cpp)
    target_link_libraries(frontend_input_tests PRIVATE charged_frontend_input)
    add_test(NAME frontend_input COMMAND frontend_input_tests)
    set_tests_properties(frontend_input PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
