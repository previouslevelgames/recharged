include_guard(GLOBAL)
# The SO backend uses POSIX sockets. On Windows the original module keeps its
# SO imports unresolved, as before this backend existed.
if(WIN32)
    return()
endif()
find_package(Threads REQUIRED)
set(MSCHARGED_NETWORK_SOURCES
    src/net/so.c
    src/net/so_host.c
    src/net/so_state.c)

function(mscharged_configure_network_target target scope)
    target_include_directories("${target}" ${scope} src
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_PREPARED}/include")
    target_compile_definitions("${target}" ${scope} MSCHARGED_NATIVE=1)
    target_link_libraries("${target}" ${scope} Threads::Threads)
    set_target_properties("${target}" PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS YES)
    add_dependencies("${target}" verify_prepared)
endfunction()

add_library(charged_network STATIC ${MSCHARGED_NETWORK_SOURCES})
mscharged_configure_network_target(charged_network PUBLIC)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_network PRIVATE
        -ffp-contract=off
        -fno-strict-aliasing
        -fsigned-char
        -Wno-unknown-pragmas
        -Wall
        -Wextra
        -Wconversion)
endif()
if(BUILD_TESTING)
    add_executable(so_backend_tests tests/so_backend.cpp tests/so_test_peer.cpp)
    target_compile_features(so_backend_tests PRIVATE cxx_std_20)
    target_link_libraries(so_backend_tests PRIVATE charged_network)
    add_test(NAME so_backend COMMAND so_backend_tests)
    set_tests_properties(so_backend PROPERTIES TIMEOUT 30)

    add_executable(so_backend_threads_tests tests/so_backend_threads.cpp tests/so_test_peer.cpp)
    target_compile_features(so_backend_threads_tests PRIVATE cxx_std_20)
    target_link_libraries(so_backend_threads_tests PRIVATE charged_network)
    add_test(NAME so_backend_threads COMMAND so_backend_threads_tests)
    set_tests_properties(so_backend_threads PROPERTIES TIMEOUT 60)

    option(MSCHARGED_TSAN_TESTS "Also run the SO backend thread tests under ThreadSanitizer" OFF)
    if(MSCHARGED_TSAN_TESTS)
        # ThreadSanitizer only sees instrumented code, so the backend sources
        # are compiled into the test instead of linking charged_network.
        add_executable(so_backend_threads_tsan_tests
            tests/so_backend_threads.cpp tests/so_test_peer.cpp ${MSCHARGED_NETWORK_SOURCES})
        mscharged_configure_network_target(so_backend_threads_tsan_tests PRIVATE)
        target_compile_features(so_backend_threads_tsan_tests PRIVATE cxx_std_20)
        target_compile_options(so_backend_threads_tsan_tests PRIVATE -fsanitize=thread -g)
        target_link_options(so_backend_threads_tsan_tests PRIVATE -fsanitize=thread)
        add_test(NAME so_backend_threads_tsan COMMAND so_backend_threads_tsan_tests)
        set_tests_properties(so_backend_threads_tsan PROPERTIES
            TIMEOUT 180
            ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
    endif()
endif()
