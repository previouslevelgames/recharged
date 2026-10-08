include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

# Platform boot metadata and explicit failures for unsupported Wii hardware /
# native title-restart requests. Original ResetTask/OSReset remain software
# owners; this target provides no game teardown or successful reboot/menu body.
add_library(charged_native_os_shutdown_requests STATIC EXCLUDE_FROM_ALL
    src/platform/os_shutdown_requests.cpp)
add_dependencies(charged_native_os_shutdown_requests verify_prepared)
target_include_directories(charged_native_os_shutdown_requests PUBLIC src)
target_compile_features(charged_native_os_shutdown_requests PUBLIC cxx_std_17)
target_link_libraries(charged_native_os_shutdown_requests PUBLIC
    charged_native_interrupts aurora::os aurora::dvd)

if(BUILD_TESTING AND TARGET charged_native_filesystem_boot AND UNIX
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_executable(native_os_shutdown_requests_tests
        tests/native_os_shutdown_requests.cpp
        src/platform/os.cpp src/platform/os_version.cpp)
    target_include_directories(native_os_shutdown_requests_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_os_shutdown_requests_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(native_os_shutdown_requests_tests PRIVATE cxx_std_17)
    target_link_libraries(native_os_shutdown_requests_tests PRIVATE
        charged_native_os_shutdown_requests charged_native_filesystem_boot)
    add_test(NAME native_os_shutdown_requests
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_os_shutdown_requests.py"
            "$<TARGET_FILE:native_os_shutdown_requests_tests>")
    set_tests_properties(native_os_shutdown_requests PROPERTIES
        TIMEOUT 30 LABELS "Platform")
endif()
