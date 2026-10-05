# Original graphics memory, matrices/state and the selected static inventory.
add_library(charged_graphics_memory STATIC
    "${MSCHARGED_PREPARED}/src/Game/GraphicsMemoryStartup.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemoryInit.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemoryNames.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLInventoryStatic.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManagerStatic.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glModelMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glState.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glStat.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glHash.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxModelMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/math.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plane.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platvmath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platqmath.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c"
    src/runtime/graphics_memory.cpp
    src/runtime/graphics_state.cpp)
add_dependencies(charged_graphics_memory verify_prepared)
target_include_directories(charged_graphics_memory PUBLIC src
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_features(charged_graphics_memory PRIVATE cxx_std_17)
set_target_properties(charged_graphics_memory PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(charged_graphics_memory PRIVATE TARGET_PC=1)
# Every legacy graphics consumer must see the same diagnostic GLView layout,
# including material units below the view library in the dependency graph.
# Original source object targets use their independent, unextended view ABI.
target_compile_definitions(charged_graphics_memory PUBLIC MSCHARGED_DIAGNOSTIC_VIEWS=1)
target_link_libraries(charged_graphics_memory PUBLIC charged_decomp_startup aurora::mtx)
# Charged's selected mtx44.c owns the projection entry points. Aurora's mtx.c
# also contains them alongside the 3x4 skin operations now needed by the port.
# Give the selected Charged providers distinct names and propagate that choice
# to their consumers. Aurora-only diagnostics retain their original provider.
# This does not depend on static archive order or weak symbols.
target_compile_definitions(charged_graphics_memory PUBLIC
    C_MTXFrustum=Charged_C_MTXFrustum C_MTXOrtho=Charged_C_MTXOrtho)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_graphics_memory PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
if(BUILD_TESTING)
    add_executable(graphics_memory_tests tests/graphics_memory.cpp)
    target_compile_features(graphics_memory_tests PRIVATE cxx_std_17)
    target_link_libraries(graphics_memory_tests PRIVATE charged_graphics_memory)
    add_test(NAME graphics_memory COMMAND graphics_memory_tests)
    set_tests_properties(graphics_memory PROPERTIES TIMEOUT 30)
    add_executable(graphics_state_tests tests/graphics_state.cpp)
    target_compile_features(graphics_state_tests PRIVATE cxx_std_17)
    target_link_libraries(graphics_state_tests PRIVATE charged_graphics_memory)
    add_test(NAME graphics_state COMMAND graphics_state_tests)
    set_tests_properties(graphics_state PROPERTIES TIMEOUT 30)
endif()
