include_guard(GLOBAL)
include(cmake/NativeFilesystem.cmake)

# Explicit native boot setup reads disc TMD title/group and installs persistent
# IOS storage. The caller supplies a retained virtual UID; original NAND/flash
# initialization and game-save decisions remain source-owned.
if(TARGET charged_native_filesystem)
    add_library(charged_native_filesystem_boot STATIC EXCLUDE_FROM_ALL
        src/platform/filesystem_boot.cpp)
    target_include_directories(charged_native_filesystem_boot PUBLIC src)
    target_compile_features(charged_native_filesystem_boot PUBLIC cxx_std_17)
    target_link_libraries(charged_native_filesystem_boot PUBLIC
        charged_native_filesystem PRIVATE charged_host)

    if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
       AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        include(cmake/OriginalOSReset.cmake)
        include(cmake/OriginalNAND.cmake)
        add_executable(original_storage_boot_tests
            tests/original_storage_boot.cpp src/platform/os.cpp
            src/platform/os_version.cpp)
        add_dependencies(original_storage_boot_tests verify_prepared)
        target_include_directories(original_storage_boot_tests PRIVATE
            "${MSCHARGED_PREPARED}/include"
            "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
            "${MSCHARGED_AURORA_PREPARED}/include")
        target_compile_definitions(original_storage_boot_tests PRIVATE
            MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
        target_compile_features(original_storage_boot_tests PRIVATE cxx_std_17)
        target_compile_options(original_storage_boot_tests PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(original_storage_boot_tests PRIVATE -Wl,--gc-sections)
        target_link_libraries(original_storage_boot_tests PRIVATE
            charged_native_filesystem_boot charged_original_os_reset
            charged_original_nand_sources charged_original_fs_sources
            charged_original_ipc_memory charged_native_ipc_boot_buffer
            nod::nod aurora::os aurora::core SDL3::SDL3)
        add_test(NAME original_storage_boot
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_storage_boot.py"
                "$<TARGET_FILE:original_storage_boot_tests>")
        set_tests_properties(original_storage_boot PROPERTIES TIMEOUT 30)
    endif()
endif()
