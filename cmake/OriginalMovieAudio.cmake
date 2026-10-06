include_guard(GLOBAL)
option(MSCHARGED_BUILD_THP_AUDIO_DIAGNOSTIC "Build the temporary original audio-only movie diagnostic" OFF)
if(NOT MSCHARGED_BUILD_THP_AUDIO_DIAGNOSTIC)
    return()
endif()

# Owner-authorized, explicitly temporary audio-only diagnostic. The normal game
# continues to request all video textures and MovieInit(1)'s real AX predecessor.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(original_movie_audio_module MODULE EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFunctionMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlBind.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFileGC.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFile.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/thp/THPSimple.cpp"
    src/platform/game_allocation_ownership.cpp
    src/platform/game_module_allocations.cpp
    src/platform/file_handle_abi.cpp
    tests/thp_audio_module.cpp)
add_dependencies(original_movie_audio_module verify_prepared)
set_target_properties(original_movie_audio_module PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(original_movie_audio_module PRIVATE tests
    "${MSCHARGED_AURORA_PREPARED}/include"
    "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(original_movie_audio_module PRIVATE charged_original_function_pool_abi)
# No runtime SDK archive enters this module: it imports the host's one foundation.
target_compile_definitions(original_movie_audio_module PRIVATE
    MSCHARGED_GAME_MODULE=1 MSCHARGED_DIAGNOSTIC_THP_AUDIO_ONLY=1
    AURORA_WII_CLOCK=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_features(original_movie_audio_module PRIVATE cxx_std_20)
target_compile_options(original_movie_audio_module PRIVATE
    -ffunction-sections -fdata-sections -fcheck-new -ffp-contract=off
    -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
target_link_options(original_movie_audio_module PRIVATE
    -Wl,--gc-sections -Wl,-Bsymbolic-functions
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/thp_audio_exports.map")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(original_movie_audio_module PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(original_movie_audio_module PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()

add_executable(mscharged-thp-audio-check
    tests/thp_audio_host.cpp src/platform/os.cpp)
add_dependencies(mscharged-thp-audio-check verify_prepared original_movie_audio_module)
target_include_directories(mscharged-thp-audio-check PRIVATE src tests)
target_compile_features(mscharged-thp-audio-check PRIVATE cxx_std_20)
target_compile_definitions(mscharged-thp-audio-check PRIVATE
    TARGET_PC=1 AURORA_WII_CLOCK=1)
target_link_options(mscharged-thp-audio-check PRIVATE
    -Wl,--export-dynamic -Wl,--wrap=SDL_PutAudioStreamDataNoCopy
    -Wl,--undefined=DVDInit -Wl,--undefined=VIInit
    -Wl,--undefined=THPInit -Wl,--undefined=THPAudioDecode
    -Wl,--undefined=ChargedNativeMetadataAllocate
    -Wl,--undefined=ChargedNativeMetadataRelease)
target_link_libraries(mscharged-thp-audio-check PRIVATE
    "-Wl,--whole-archive" aurora::os "-Wl,--no-whole-archive"
    charged_native_ai charged_native_metadata charged_thp_decoder
    aurora::dvd aurora::vi aurora::core SDL3::SDL3 ${CMAKE_DL_LIBS})
if(BUILD_TESTING AND NOT MSCHARGED_BUILD_GX_CHECK AND NOT MSCHARGED_BUILD_SCENE_PREVIEW)
    add_test(NAME original_movie_audio COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_movie_audio.py"
        "$<TARGET_FILE:mscharged-thp-audio-check>" "$<TARGET_FILE:original_movie_audio_module>")
    set_tests_properties(original_movie_audio PROPERTIES TIMEOUT 40 LABELS "Diagnostic")
endif()
