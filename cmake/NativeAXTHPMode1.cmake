include_guard(GLOBAL)
option(MSCHARGED_BUILD_AX_THP_MODE1_DIAGNOSTIC
    "Build the bounded original AX/THPSimple mode1 audio qualifier" OFF)
if(NOT MSCHARGED_BUILD_AX_THP_MODE1_DIAGNOSTIC OR NOT BUILD_TESTING
        OR NOT TARGET native_ax_active_source_fixture)
    return()
endif()
include(cmake/NativeAXNormalCommand.cmake)
include(cmake/NativeThpDecoder.cmake)
include(cmake/OriginalFunctionPools.cmake)
include(cmake/OriginalCompressedFiles.cmake)

# Separate terminal leaf: retain whole original NL/THP methods, source operators
# and pools. Host owns the sole SDK. The named audio-only scope holds video
# texture/decoder work; this never replaces the normal MovieInit(1) game path.
add_library(native_ax_thp_source_fixture MODULE
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
    tests/ax_thp_mode1_module.cpp)
mscharged_add_original_inflater(native_ax_thp_source_fixture)
add_dependencies(native_ax_thp_source_fixture verify_prepared)
target_include_directories(native_ax_thp_source_fixture PRIVATE tests
    "${MSCHARGED_AURORA_PREPARED}/include"
    "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(native_ax_thp_source_fixture PRIVATE charged_original_function_pool_abi)
target_compile_definitions(native_ax_thp_source_fixture PRIVATE
    MSCHARGED_GAME_MODULE=1 MSCHARGED_DIAGNOSTIC_THP_AUDIO_ONLY=1
    AURORA_WII_CLOCK=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_features(native_ax_thp_source_fixture PRIVATE cxx_std_20)
target_compile_options(native_ax_thp_source_fixture PRIVATE
    -fcheck-new -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(native_ax_thp_source_fixture PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(native_ax_thp_source_fixture PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
set_target_properties(native_ax_thp_source_fixture PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_options(native_ax_thp_source_fixture PRIVATE
    -Wl,--no-gc-sections -Wl,-Bsymbolic-functions
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/ax_thp_mode1_exports.map")

add_executable(native_ax_thp_mode1_tests tests/native_ax_thp_mode1.cpp
    src/platform/os.cpp src/platform/os_version.cpp)
add_dependencies(native_ax_thp_mode1_tests verify_prepared native_ax_thp_source_fixture
    native_ax_active_source_fixture native_ax_normal_os_source_fixture)
target_include_directories(native_ax_thp_mode1_tests PRIVATE tests
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(native_ax_thp_mode1_tests PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(native_ax_thp_mode1_tests PRIVATE cxx_std_20)
target_compile_options(native_ax_thp_mode1_tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
target_link_libraries(native_ax_thp_mode1_tests PRIVATE charged_native_ax_normal_command
    charged_native_os_audio_boot charged_native_thread_queues charged_native_ai
    charged_native_metadata charged_thp_decoder aurora::dvd aurora::vi aurora::os
    SDL3::SDL3 ${CMAKE_DL_LIBS})
get_target_property(ax_source_exports native_ax_normal_tests LINK_OPTIONS)
target_link_options(native_ax_thp_mode1_tests PRIVATE ${ax_source_exports}
    -Wl,--wrap=SDL_PutAudioStreamDataNoCopy
    -Wl,--undefined=DVDInit -Wl,--undefined=DVDConvertPathToEntrynum
    -Wl,--undefined=DVDFastOpen -Wl,--undefined=DVDReadAsyncPrio
    -Wl,--undefined=DVDClose -Wl,--undefined=DVDGetCommandBlockStatus
    -Wl,--undefined=DVDGetDriveStatus -Wl,--undefined=VIInit
    -Wl,--undefined=LCEnable -Wl,--undefined=LCDisable
    -Wl,--undefined=THPInit -Wl,--undefined=THPAudioDecode
    -Wl,--undefined=AIGetDMAStartAddr -Wl,--undefined=AIGetDSPSampleRate
    -Wl,--undefined=OSYieldThread -Wl,--undefined=OSGetConsoleSimulatedMem2Size
    -Wl,--undefined=OSInitAlloc -Wl,--undefined=OSCreateHeap -Wl,--undefined=OSSetCurrentHeap
    -Wl,--undefined=ChargedNativeMetadataAllocate -Wl,--undefined=ChargedNativeMetadataRelease)
if(NOT MSCHARGED_BUILD_GX_CHECK)
    add_test(NAME native_ax_thp_mode1 COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_native_ax_thp_mode1.py"
        "$<TARGET_FILE:native_ax_thp_mode1_tests>"
        "$<TARGET_FILE:native_ax_active_source_fixture>"
        "$<TARGET_FILE:native_ax_normal_os_source_fixture>"
        "$<TARGET_FILE:native_ax_thp_source_fixture>")
    set_tests_properties(native_ax_thp_mode1 PROPERTIES TIMEOUT 45
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" LABELS "Diagnostic")
endif()
