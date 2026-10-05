# Legacy diagnostics use the same real core services as the original source.
# Keep their extracted entry and setup/check helpers out of the core library.
include_guard(GLOBAL)
include(cmake/OriginalCore.cmake)
include(cmake/OriginalFontTextures.cmake)
include(cmake/OriginalGraphics.cmake)
include(cmake/OriginalViews.cmake)
include(cmake/OriginalTiming.cmake)
include(cmake/OriginalTweaks.cmake)
add_library(charged_decomp_startup STATIC
    "${MSCHARGED_PREPARED}/src/Game/Startup.cpp"
    src/runtime/startup_animation.cpp
    src/runtime/startup_memory.cpp
    src/runtime/startup_files.cpp
    src/runtime/startup_context.cpp)
add_dependencies(charged_decomp_startup verify_prepared)
target_link_libraries(charged_decomp_startup PUBLIC charged_original_core
    PRIVATE aurora::os aurora::dvd SDL3::SDL3)
target_compile_definitions(charged_decomp_startup PRIVATE TARGET_PC=1)
target_compile_features(charged_decomp_startup PRIVATE cxx_std_17)
target_include_directories(charged_decomp_startup PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_decomp_startup PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
include(cmake/GraphicsMemory.cmake)
include(cmake/EffectsVertex.cmake)
