include_guard(GLOBAL)
include(cmake/NativeIOS.cmake)
include(cmake/OriginalFS.cmake)
include(cmake/OriginalIPCMemory.cmake)
include(cmake/NativeIPCBootBuffer.cmake)

# Actual persistent IOS FS/ES backing. The current backend uses POSIX file APIs;
# Windows needs its own physical provider. Installation is explicit native boot
# setup, before the original NAND initializer; this supplies no game save data.
if(UNIX)
    add_library(charged_native_filesystem STATIC EXCLUDE_FROM_ALL
        src/platform/filesystem_device.cpp)
    add_dependencies(charged_native_filesystem verify_prepared)
    target_include_directories(charged_native_filesystem PUBLIC src)
    target_compile_features(charged_native_filesystem PUBLIC cxx_std_17)
    target_link_libraries(charged_native_filesystem PUBLIC charged_native_ios)

    if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
       AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_executable(native_filesystem_tests
            tests/original_filesystem.cpp src/platform/os.cpp)
        add_dependencies(native_filesystem_tests verify_prepared)
        target_include_directories(native_filesystem_tests PRIVATE
            "${MSCHARGED_PREPARED}/include"
            "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
            "${MSCHARGED_AURORA_PREPARED}/include")
        target_compile_definitions(native_filesystem_tests PRIVATE
            MSCHARGED_NATIVE=1 TARGET_PC=1)
        target_compile_features(native_filesystem_tests PRIVATE cxx_std_17)
        target_compile_options(native_filesystem_tests PRIVATE
            -ffunction-sections -fdata-sections)
        target_link_options(native_filesystem_tests PRIVATE -Wl,--gc-sections)
        target_link_libraries(native_filesystem_tests PRIVATE
            charged_native_filesystem charged_original_fs_sources
            charged_original_ipc_memory charged_native_ipc_boot_buffer)
        add_test(NAME native_filesystem COMMAND native_filesystem_tests
            "${CMAKE_CURRENT_BINARY_DIR}/native-filesystem-tests-data")
        set_tests_properties(native_filesystem PROPERTIES TIMEOUT 30)
    endif()
endif()
