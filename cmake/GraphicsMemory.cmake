# Original graphics memory plus the explicitly selected static inventory.
add_library(charged_graphics_memory STATIC
    "${MSCHARGED_PREPARED}/src/Game/GraphicsMemoryStartup.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemoryInit.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemoryNames.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLInventoryStatic.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManagerStatic.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
    src/runtime/graphics_memory.cpp)
add_dependencies(charged_graphics_memory verify_prepared)
target_include_directories(charged_graphics_memory PUBLIC src
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_features(charged_graphics_memory PRIVATE cxx_std_17)
target_link_libraries(charged_graphics_memory PUBLIC charged_decomp_startup)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_graphics_memory PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
if(BUILD_TESTING)
    add_executable(graphics_memory_tests tests/graphics_memory.cpp)
    target_compile_features(graphics_memory_tests PRIVATE cxx_std_17)
    target_link_libraries(graphics_memory_tests PRIVATE charged_graphics_memory)
    add_test(NAME graphics_memory COMMAND graphics_memory_tests)
    set_tests_properties(graphics_memory PROPERTIES TIMEOUT 30)
endif()
