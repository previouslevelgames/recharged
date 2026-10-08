include_guard(GLOBAL)
include(cmake/OriginalOSMessages.cmake)
# Real native SDK worker lifetimes share the existing interrupt/queue registry.
# The fixture runs the complete original message-ring TU through those workers.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(native_thread_lifecycle_tests tests/native_thread_lifecycle.cpp)
    target_compile_features(native_thread_lifecycle_tests PRIVATE cxx_std_17)
    target_link_libraries(native_thread_lifecycle_tests PRIVATE charged_original_os_messages)
    add_test(NAME native_thread_lifecycle COMMAND native_thread_lifecycle_tests)
    set_tests_properties(native_thread_lifecycle PROPERTIES TIMEOUT 20)
    add_executable(native_thread_scheduler_tests tests/native_thread_scheduler.cpp)
    target_compile_features(native_thread_scheduler_tests PRIVATE cxx_std_17)
    target_link_libraries(native_thread_scheduler_tests PRIVATE charged_original_os_messages)
    add_test(NAME native_thread_scheduler COMMAND native_thread_scheduler_tests)
    set_tests_properties(native_thread_scheduler PROPERTIES TIMEOUT 15)
    # Initial higher-priority execution ends only at a real block or return.
    # The same sole registry/message provider is used by the existing tests.
    add_executable(native_thread_initial_resume_tests tests/native_thread_initial_resume.cpp)
    target_compile_features(native_thread_initial_resume_tests PRIVATE cxx_std_17)
    target_link_libraries(native_thread_initial_resume_tests PRIVATE charged_original_os_messages)
    add_test(NAME native_thread_initial_resume COMMAND native_thread_initial_resume_tests)
    set_tests_properties(native_thread_initial_resume PROPERTIES TIMEOUT 15)
    # Whole original lock/wait/unlock decisions use the same sole native kernel
    # registry, including real inherited-priority and held-lock wait lifetimes.
    add_executable(original_os_mutex_tests tests/original_os_mutex.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/os/OSMutex.c")
    add_dependencies(original_os_mutex_tests verify_prepared)
    target_compile_features(original_os_mutex_tests PRIVATE cxx_std_17 c_std_17)
    target_link_libraries(original_os_mutex_tests PRIVATE charged_original_os_messages)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(original_os_mutex_tests PRIVATE
            "$<$<COMPILE_LANGUAGE:C>:-fexceptions;-fno-strict-aliasing;-Wno-unknown-pragmas>")
    endif()
    add_test(NAME original_os_mutex COMMAND original_os_mutex_tests)
    set_tests_properties(original_os_mutex PROPERTIES TIMEOUT 15)
    add_test(NAME original_os_mutex_owner_retirement
        COMMAND original_os_mutex_tests --retire-held-mutex)
    set_tests_properties(original_os_mutex_owner_retirement PROPERTIES TIMEOUT 10)
endif()
