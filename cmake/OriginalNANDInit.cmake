include_guard(GLOBAL)
include(cmake/OriginalOSReset.cmake)
include(cmake/NativeFilesystem.cmake)
include(cmake/OriginalNAND.cmake)

# Registration/NAND init and raw requests are measured separately from game
# save readiness, banner/Mii formats and the full SRAM/reset/shutdown graph.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_executable(original_nand_init_tests tests/original_nand_init.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    add_dependencies(original_nand_init_tests verify_prepared)
    target_include_directories(original_nand_init_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(original_nand_init_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
    target_compile_features(original_nand_init_tests PRIVATE cxx_std_17)
    target_compile_options(original_nand_init_tests PRIVATE
        -ffunction-sections -fdata-sections)
    target_link_options(original_nand_init_tests PRIVATE -Wl,--gc-sections)
    target_link_libraries(original_nand_init_tests PRIVATE
        charged_original_os_reset charged_original_nand_sources
        charged_original_fs_sources charged_native_filesystem
        charged_original_ipc_memory charged_native_ipc_boot_buffer
        aurora::os aurora::core SDL3::SDL3)
    add_test(NAME original_nand_init COMMAND original_nand_init_tests
        "${CMAKE_CURRENT_BINARY_DIR}/original-nand-init-tests-data")
    set_tests_properties(original_nand_init PROPERTIES TIMEOUT 30)
endif()
