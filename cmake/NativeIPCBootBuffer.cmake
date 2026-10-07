include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

# The original SDK owns initialization and the advancing IPC arena cursor.
# Native boot metadata supplies full-width pointers; MMIO remains unsupported.
add_library(charged_native_ipc_boot_buffer STATIC
    src/platform/ipc_boot_buffer.cpp
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSIpc.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ipc/ipcMain.c")
add_dependencies(charged_native_ipc_boot_buffer verify_prepared)
target_include_directories(charged_native_ipc_boot_buffer PUBLIC src PRIVATE
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_ipc_boot_buffer PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_native_ipc_boot_buffer PRIVATE c_std_17 cxx_std_17)
target_link_libraries(charged_native_ipc_boot_buffer PUBLIC charged_native_interrupts)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_native_ipc_boot_buffer PRIVATE
        -fexceptions -fno-strict-aliasing -Wno-unknown-pragmas
        $<$<COMPILE_LANGUAGE:C>:-Werror=pointer-to-int-cast>)
endif()

if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(native_ipc_boot_buffer_tests tests/native_ipc_boot_buffer.cpp)
    target_include_directories(native_ipc_boot_buffer_tests PRIVATE
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_ipc_boot_buffer_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_link_libraries(native_ipc_boot_buffer_tests PRIVATE
        charged_native_ipc_boot_buffer charged_original_ipc_memory)
    add_test(NAME native_ipc_boot_buffer COMMAND native_ipc_boot_buffer_tests)
    set_tests_properties(native_ipc_boot_buffer PROPERTIES TIMEOUT 15)
endif()
