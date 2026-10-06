include_guard(GLOBAL)
# Inventory teardown owns GLVertexAnim even in graphics-memory-only builds.
target_sources(charged_graphics_memory PRIVATE
    "${MSCHARGED_PREPARED}/src/Game/GL/GLVertexAnim.cpp")
# Historical frame-binding diagnostics consume host-format headers and the
# extracted provider below; whole original modules never define this scope.
target_compile_definitions(charged_graphics_memory PRIVATE MSCHARGED_DIAGNOSTIC_VERTEX_ANIMATION=1)
if(MSCHARGED_BUILD_SCENE_PREVIEW)
    add_library(charged_effects_vertex STATIC src/runtime/effects_vertex.cpp
        "${MSCHARGED_PREPARED}/src/Game/GL/GLVertexAnimModel.cpp")
    target_compile_definitions(charged_effects_vertex PRIVATE MSCHARGED_DIAGNOSTIC_VERTEX_ANIMATION=1)
    add_dependencies(charged_effects_vertex verify_prepared)
    target_compile_features(charged_effects_vertex PUBLIC cxx_std_20)
    target_include_directories(charged_effects_vertex PUBLIC src
        PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(charged_effects_vertex PUBLIC charged_static_inventory charged_shadows charged_frames)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        target_compile_options(charged_effects_vertex PRIVATE -ffp-contract=off)
    endif()
    if(BUILD_TESTING)
        add_executable(effects_vertex_tests tests/effects_vertex.cpp)
        target_link_libraries(effects_vertex_tests PRIVATE charged_effects_vertex)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
            target_compile_options(effects_vertex_tests PRIVATE -ffp-contract=off)
        endif()
        add_test(NAME effects_vertex COMMAND effects_vertex_tests)
        set_tests_properties(effects_vertex PROPERTIES TIMEOUT 60)
    endif()
endif()
