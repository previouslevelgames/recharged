include_guard(GLOBAL)

# The disc reader for builds without nod, whose Rust has no standard library
# on the Switch: nod's C API (nod.h) in C++, for Wii ISO and WBFS images, split
# ones too. Other formats (RVZ, WIA, CISO, GCZ ...) and GameCube discs fail
# with NOD_RESULT_ERR_FORMAT. It links in place of nod::nod and keeps its own
# copy of the prepared nod.h (patches/nod add nod_try_read_cached_at), since
# those builds do not prepare nod; the test checks the copy where nod is prepared.
set(_wii_disc_root "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(charged_wii_disc STATIC
    "${_wii_disc_root}/src/platform/wii_disc/aes128.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/disc.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/image.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/nod_api.cpp"
)
target_include_directories(charged_wii_disc PUBLIC "${_wii_disc_root}/src/platform/wii_disc/include")
target_compile_features(charged_wii_disc PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(charged_wii_disc PUBLIC Threads::Threads)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(wii_disc_reader_tests "${_wii_disc_root}/tests/wii_disc_reader.cpp")
    target_include_directories(wii_disc_reader_tests PRIVATE "${_wii_disc_root}/src")
    target_compile_features(wii_disc_reader_tests PRIVATE cxx_std_17)
    target_link_libraries(wii_disc_reader_tests PRIVATE charged_wii_disc)
    set(_wii_disc_header_check)
    if(MSCHARGED_NOD_PREPARED)
        set(_wii_disc_header_check "${_wii_disc_root}/src/platform/wii_disc/include/nod.h"
            "${MSCHARGED_NOD_PREPARED}/nod-ffi/include/nod.h")
    endif()
    add_test(NAME wii_disc_reader
        COMMAND wii_disc_reader_tests "${CMAKE_CURRENT_BINARY_DIR}/wii_disc_reader" ${_wii_disc_header_check})
    set_tests_properties(wii_disc_reader PROPERTIES TIMEOUT 120)
endif()
