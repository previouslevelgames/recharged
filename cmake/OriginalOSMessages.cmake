include_guard(GLOBAL)
include(cmake/NativeThreadQueues.cmake)
# Complete original Matching SDK source. Native adaptation changes only live
# pointer-cell width; original ring/block/wake decisions stay in this TU.
add_library(charged_original_os_messages STATIC
    "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSMessage.c")
add_dependencies(charged_original_os_messages verify_prepared)
target_include_directories(charged_original_os_messages PUBLIC
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_os_messages PUBLIC MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_os_messages PRIVATE c_std_17)
target_link_libraries(charged_original_os_messages PUBLIC charged_native_thread_queues)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(charged_original_os_messages PRIVATE
        -fexceptions -fno-strict-aliasing -Wno-unknown-pragmas)
endif()
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(original_os_message_tests tests/original_os_messages.cpp)
    target_compile_features(original_os_message_tests PRIVATE cxx_std_17)
    target_link_libraries(original_os_message_tests PRIVATE charged_original_os_messages)
    add_test(NAME original_os_messages COMMAND original_os_message_tests)
    set_tests_properties(original_os_messages PROPERTIES TIMEOUT 15)
endif()
