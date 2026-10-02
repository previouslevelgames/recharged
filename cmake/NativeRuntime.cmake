# Shared, explicitly selected original core, allocator and Wii/NL file services.
include_guard(GLOBAL)
add_library(charged_sanim_decode STATIC
    "${MSCHARGED_PREPARED}/src/Game/SAnimDecode.cpp"
    src/runtime/sanim_decode.cpp)
add_dependencies(charged_sanim_decode verify_prepared)
target_include_directories(charged_sanim_decode PUBLIC "${MSCHARGED_PREPARED}/include"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_sanim_decode PUBLIC MSCHARGED_NATIVE=1)
target_compile_features(charged_sanim_decode PRIVATE cxx_std_17)

add_library(charged_native_allocator STATIC "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp")
add_dependencies(charged_native_allocator verify_prepared)
target_include_directories(charged_native_allocator PUBLIC "${MSCHARGED_PREPARED}/include"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_allocator PRIVATE MSCHARGED_NATIVE=1)
target_compile_features(charged_native_allocator PRIVATE cxx_std_17)

add_library(charged_decomp_startup STATIC
    "${MSCHARGED_PREPARED}/src/Game/Startup.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlInit.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTicker.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTime.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glPlatPreStartup.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFileGC.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFileBasic.cpp"
    src/runtime/startup_animation.cpp
    src/runtime/startup_memory.cpp
    src/runtime/function_memory.cpp
    src/runtime/startup_files.cpp
    src/runtime/whole_file.cpp
    src/runtime/startup_os.cpp
    src/runtime/startup_context.cpp
)
add_dependencies(charged_decomp_startup verify_prepared)
target_include_directories(charged_decomp_startup PUBLIC
    "${MSCHARGED_PREPARED}/include"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(charged_decomp_startup PUBLIC MSCHARGED_NATIVE=1)
target_compile_features(charged_decomp_startup PRIVATE cxx_std_17)
target_link_libraries(charged_decomp_startup PRIVATE charged_foundation charged_native_allocator
    charged_sanim_decode aurora::dvd aurora::os aurora::vi aurora::core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_decomp_startup PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
    target_compile_options(charged_native_allocator PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
    target_compile_options(charged_sanim_decode PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()

include(cmake/GraphicsMemory.cmake)
