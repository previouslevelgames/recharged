include_guard(GLOBAL)
include(cmake/NativeInterrupts.cmake)

# Whole original CPU heap implementation. This does not provide IPC MMIO,
# an IOS device, a save filesystem or a successful NAND initialization.
add_library(charged_original_ipc_memory STATIC
    "${MSCHARGED_PREPARED}/src/RVL_SDK/ipc/memory.c")
add_dependencies(charged_original_ipc_memory verify_prepared)
target_include_directories(charged_original_ipc_memory PUBLIC
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_ipc_memory PUBLIC
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_ipc_memory PRIVATE c_std_17)
target_link_libraries(charged_original_ipc_memory PUBLIC charged_native_interrupts)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_original_ipc_memory PRIVATE
        -fexceptions -fno-strict-aliasing -Werror=pointer-to-int-cast)
endif()

if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(original_ipc_memory_tests tests/original_ipc_memory.cpp)
    target_link_libraries(original_ipc_memory_tests PRIVATE charged_original_ipc_memory)
    target_compile_features(original_ipc_memory_tests PRIVATE cxx_std_17)
    add_test(NAME original_ipc_memory COMMAND original_ipc_memory_tests)
    set_tests_properties(original_ipc_memory PROPERTIES TIMEOUT 15)
endif()
