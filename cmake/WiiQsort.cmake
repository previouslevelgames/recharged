include_guard(GLOBAL)
# The Wii's qsort (MSL heapsort) under its own name. Host qsorts order equal
# elements differently (glibc, UCRT, macOS libc), and online lockstep needs
# every machine to sort as the Wii: the online draft orders tied teams with it.
add_library(charged_wii_qsort STATIC "${MSCHARGED_PREPARED}/src/MSL/qsort.c")
add_dependencies(charged_wii_qsort verify_prepared)
target_include_directories(charged_wii_qsort PRIVATE
    "${MSCHARGED_PREPARED}/libs/MSL_C/include")
target_compile_definitions(charged_wii_qsort PRIVATE
    MSCHARGED_NATIVE=1 qsort=ChargedWii_qsort)
target_compile_options(charged_wii_qsort PRIVATE -fno-strict-aliasing)
set_target_properties(charged_wii_qsort PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(charged_wii_qsort INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(BUILD_TESTING)
    add_executable(wii_qsort_tests tests/wii_qsort.cpp)
    target_compile_features(wii_qsort_tests PRIVATE cxx_std_17)
    target_link_libraries(wii_qsort_tests PRIVATE charged_wii_qsort)
    add_test(NAME wii_qsort COMMAND wii_qsort_tests)
    set_tests_properties(wii_qsort PROPERTIES TIMEOUT 10)
endif()
