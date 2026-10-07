include_guard(GLOBAL)
include(cmake/NativeThreadQueues.cmake)

# Whole source owns its priority/FIFO shutdown registrations. The native active
# thread list is the existing SDK registry, not a second low-memory overlay.
# SRAM, reset devices and full shutdown remain separate unresolved providers.
add_library(charged_original_os_reset STATIC EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSReset.c")
add_dependencies(charged_original_os_reset verify_prepared)
target_include_directories(charged_original_os_reset PUBLIC src
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_os_reset PUBLIC
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_os_reset PRIVATE c_std_17)
target_link_libraries(charged_original_os_reset PUBLIC
    charged_native_thread_queues PRIVATE aurora::os)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_original_os_reset PRIVATE
        -fexceptions -ffunction-sections -fdata-sections
        -fno-strict-aliasing -Werror=pointer-to-int-cast
        -Werror=implicit-function-declaration -Wno-unknown-pragmas)
endif()
