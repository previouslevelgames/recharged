include(cmake/NativeSystemSettings.cmake)
# Original source and native platform services. This library deliberately has
# no extracted game entry, game globals or startup diagnostic owners.
include_guard(GLOBAL)
add_library(charged_sanim_decode STATIC
    "${MSCHARGED_PREPARED}/src/Game/SAnimDecode.cpp")
add_dependencies(charged_sanim_decode verify_prepared)
target_include_directories(charged_sanim_decode PUBLIC "${MSCHARGED_PREPARED}/include"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(charged_sanim_decode PUBLIC MSCHARGED_NATIVE=1)
target_compile_features(charged_sanim_decode PRIVATE cxx_std_17)

# This provider belongs to the host foundation, outside any original-game module.
# Game ownership bookkeeping imports its C interface instead of game operators.
add_library(charged_native_metadata STATIC src/platform/host_metadata.cpp)
target_include_directories(charged_native_metadata PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(charged_native_metadata PRIVATE cxx_std_17)

add_library(charged_native_allocator STATIC "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
    src/platform/game_allocation_ownership.cpp)
add_dependencies(charged_native_allocator verify_prepared)
target_include_directories(charged_native_allocator PUBLIC "${MSCHARGED_PREPARED}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_allocator PRIVATE MSCHARGED_NATIVE=1)
target_compile_features(charged_native_allocator PRIVATE cxx_std_17)
target_link_libraries(charged_native_allocator PRIVATE charged_native_metadata)

add_library(charged_original_core STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlInit.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTicker.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTime.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glPlatPreStartup.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFileGC.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFileBasic.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlString.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
    src/runtime/function_memory.cpp
    src/runtime/whole_file.cpp
    src/platform/os.cpp
    src/platform/tweak_storage.cpp
)
add_dependencies(charged_original_core verify_prepared)
target_include_directories(charged_original_core PUBLIC
    "${MSCHARGED_PREPARED}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MSCHARGED_AURORA_PREPARED}/include"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_core PUBLIC MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_core PRIVATE cxx_std_17)
target_link_libraries(charged_original_core PUBLIC aurora::os
    PRIVATE charged_foundation charged_native_allocator
    charged_sanim_decode charged_native_interrupts aurora::dvd aurora::vi aurora::core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    foreach(target IN ITEMS charged_original_core charged_native_allocator charged_sanim_decode)
        target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
    endforeach()
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(charged_sanim_decode PRIVATE -Wno-register)
endif()

target_link_libraries(charged_original_core PUBLIC charged_native_system_settings)
