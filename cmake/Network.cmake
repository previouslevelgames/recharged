include_guard(GLOBAL)
# The SO backend uses POSIX sockets. On Windows the original module keeps its
# SO imports unresolved, as before this backend existed.
if(WIN32)
    return()
endif()
add_library(charged_network STATIC
    src/net/so.c
    src/net/so_host.c)
add_dependencies(charged_network verify_prepared)
target_include_directories(charged_network PUBLIC src
    PUBLIC "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    PUBLIC "${MSCHARGED_PREPARED}/include")
set_target_properties(charged_network PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES)
target_compile_definitions(charged_network PUBLIC MSCHARGED_NATIVE=1)
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
endif()
