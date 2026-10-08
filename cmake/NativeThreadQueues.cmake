if(TARGET charged_native_thread_queues)
    return()
endif()
include(cmake/NativeInterrupts.cmake)
# Reuse the existing actual host-stack provider without duplicating its symbol.
if(NOT TARGET charged_platform_thread)
    add_library(charged_platform_thread STATIC src/platform/thread.cpp)
    target_include_directories(charged_platform_thread PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(charged_platform_thread PUBLIC cxx_std_17)
    target_link_libraries(charged_platform_thread PUBLIC Threads::Threads)
endif()
add_library(charged_native_thread_queues STATIC src/platform/thread_queues.cpp)
add_dependencies(charged_native_thread_queues verify_prepared)
target_include_directories(charged_native_thread_queues PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_thread_queues PUBLIC TARGET_PC=1)
target_compile_features(charged_native_thread_queues PUBLIC cxx_std_17)
target_link_libraries(charged_native_thread_queues PUBLIC
    charged_native_interrupts charged_platform_thread)
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(native_thread_queue_tests tests/native_thread_queues.cpp)
    target_link_libraries(native_thread_queue_tests PRIVATE charged_native_thread_queues)
    target_compile_features(native_thread_queue_tests PRIVATE cxx_std_17)
    add_test(NAME native_thread_queues COMMAND native_thread_queue_tests)
    set_tests_properties(native_thread_queues PROPERTIES TIMEOUT 15)
    # Qualify the actual provider in original-main builds as well as the
    # legacy startup profile, using the complete original message-ring TU.
    include(cmake/NativeThreadLifecycle.cmake)
endif()
