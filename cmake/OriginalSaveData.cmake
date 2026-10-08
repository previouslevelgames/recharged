include_guard(GLOBAL)

# Original ordinary save byte transport: retained Cup/Striker serializers,
# source CRC and actual owned NAND/flash callbacks. Full SaveLoad/popups/flags,
# online records, original main/tasks and native VI/GX remain separate gates.
if(NOT BUILD_TESTING OR MSCHARGED_BUILD_GX_CHECK
        OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
        OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/NativeFilesystemBoot.cmake)
include(cmake/OriginalOSReset.cmake)
include(cmake/OriginalNAND.cmake)

add_library(original_save_data_source_fixture MODULE
    "${MSCHARGED_PREPARED}/src/NL/plat/nlFlash.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AI/StatsGatherer.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlString.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMain.cpp"
    "${MSCHARGED_PREPARED}/src/Game/DB/UserOptions.cpp"
    "${MSCHARGED_PREPARED}/src/Game/DB/BasicGameInfo.cpp"
    "${MSCHARGED_PREPARED}/src/Game/DB/StrikerChallenge.cpp"
    src/platform/save_data.cpp
    src/platform/game_allocation_ownership.cpp
    src/platform/game_module_allocations.cpp
    tests/fixtures/original_save_data_module.cpp)
add_dependencies(original_save_data_source_fixture verify_prepared)
set_target_properties(original_save_data_source_fixture PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(original_save_data_source_fixture PRIVATE src
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(original_save_data_source_fixture PRIVATE
    MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1
    AURORA_WII_CLOCK=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_features(original_save_data_source_fixture PRIVATE cxx_std_20)
target_compile_options(original_save_data_source_fixture PRIVATE
    -fno-rtti -ffunction-sections -fdata-sections -fno-strict-aliasing
    -ffp-contract=off -fsigned-char -fcheck-new -Wno-unknown-pragmas)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(original_save_data_source_fixture PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(original_save_data_source_fixture PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
# One SDK remains in the host. Retain the original task-base method in its
# upstream StatsGatherer.cpp owner, without extracting or replacing its body.
target_link_options(original_save_data_source_fixture PRIVATE
    -Wl,--gc-sections -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)

add_executable(original_save_data_tests tests/original_save_data.cpp
    src/platform/os.cpp src/platform/os_version.cpp)
add_dependencies(original_save_data_tests verify_prepared original_save_data_source_fixture)
target_include_directories(original_save_data_tests PRIVATE src
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(original_save_data_tests PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_compile_features(original_save_data_tests PRIVATE cxx_std_20)
target_compile_options(original_save_data_tests PRIVATE
    -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off)
target_link_options(original_save_data_tests PRIVATE -Wl,--gc-sections)
# Explicit roots keep absent uncalled NANDMove/MEMCLR outside this selected
# scope; --export-dynamic would defeat the section-collection boundary.
foreach(symbol IN ITEMS
    ChargedNativeMetadataAllocate
    ChargedNativeMetadataRelease
    DVDInit
    SaveGateCheck
    SaveGateIOSPending
    SaveGateOwnerContextRestored
    SaveGateServiceIOS
    NANDChangeDir
    NANDChangeDirAsync
    NANDCheck
    NANDCheckAsync
    NANDClose
    NANDCloseAsync
    NANDCreate
    NANDCreateAsync
    NANDCreateDir
    NANDCreateDirAsync
    NANDDelete
    NANDDeleteAsync
    NANDGetCurrentDir
    NANDGetHomeDir
    NANDGetLength
    NANDGetLengthAsync
    NANDInit
    NANDOpen
    NANDOpenAsync
    NANDRead
    NANDReadAsync
    NANDWrite
    NANDWriteAsync
    OSAllocFromMEM1ArenaLo
    OSAllocFromMEM2ArenaLo
    OSCreateHeap
    OSGetConsoleSimulatedMem2Size
    OSGetMEM1ArenaHi
    OSGetMEM1ArenaLo
    OSGetMEM2ArenaHi
    OSGetMEM2ArenaLo
    OSInit
    OSInitAlloc
    OSReport
    OSSetCurrentHeap
    VIInit
    nandIsInitialized
    )
    target_link_options(original_save_data_tests PRIVATE
        "-Wl,--export-dynamic-symbol=${symbol}"
        "-Wl,--undefined=${symbol}")
endforeach()
target_link_libraries(original_save_data_tests PRIVATE
    charged_native_filesystem_boot charged_original_os_reset
    charged_original_nand_sources charged_original_fs_sources
    charged_original_ipc_memory charged_native_ipc_boot_buffer
    charged_native_metadata aurora::dvd aurora::vi aurora::os aurora::core
    SDL3::SDL3 ${CMAKE_DL_LIBS})
add_test(NAME original_save_data COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_save_data.py"
    "$<TARGET_FILE:original_save_data_tests>"
    "$<TARGET_FILE:original_save_data_source_fixture>")
set_tests_properties(original_save_data PROPERTIES TIMEOUT 30 LABELS "Platform")
