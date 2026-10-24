include_guard(GLOBAL)

# The disc reader for builds without nod, whose Rust has no standard library
# on the Switch: nod's C API (nod.h) in C++, for Wii ISO, WBFS, RVZ and WIA
# images, split ones too. Other formats (CISO, GCZ ...), WIA's purge
# compression, WIA/RVZ chunks over 32 MiB and GameCube discs fail with
# NOD_RESULT_ERR_FORMAT. It links in place of nod::nod and keeps its own copy
# of the prepared nod.h (patches/nod add nod_try_read_cached_at), since those
# builds do not prepare nod; the test checks the copy where nod is prepared.
set(_wii_disc_root "${CMAKE_CURRENT_LIST_DIR}/..")

# RVZ and WIA codecs. Zstandard: the build's prepared zstd where it has one
# (the Switch and the Aurora host define zstd::libzstd; the Switch does so
# after including this), else the system's. bzip2 and liblzma: the system's,
# on the Switch devkitPro's portlibs. The Switch needs them; elsewhere nod
# reads the discs and only the test builds this reader, when they are there.
set(_wii_disc_required)
if(NINTENDO_SWITCH)
    set(_wii_disc_required REQUIRED)
endif()
find_package(BZip2 ${_wii_disc_required})
find_package(LibLZMA ${_wii_disc_required})
set(_wii_disc_zstd)
if(NINTENDO_SWITCH OR TARGET zstd::libzstd)
    set(_wii_disc_zstd zstd::libzstd)
else()
    find_path(MSCHARGED_ZSTD_INCLUDE_DIR zstd.h)
    find_library(MSCHARGED_ZSTD_LIBRARY NAMES zstd)
    if(MSCHARGED_ZSTD_INCLUDE_DIR AND MSCHARGED_ZSTD_LIBRARY)
        add_library(charged_wii_disc_zstd UNKNOWN IMPORTED)
        set_target_properties(charged_wii_disc_zstd PROPERTIES IMPORTED_LOCATION "${MSCHARGED_ZSTD_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MSCHARGED_ZSTD_INCLUDE_DIR}")
        set(_wii_disc_zstd charged_wii_disc_zstd)
    endif()
endif()
if(NOT BZIP2_FOUND OR NOT LIBLZMA_FOUND OR NOT _wii_disc_zstd)
    message(STATUS "Wii disc reader test: skipped, it needs the zstd, bzip2 and liblzma headers")
    return()
endif()

add_library(charged_wii_disc STATIC
    "${_wii_disc_root}/src/platform/wii_disc/aes128.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/disc.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/image.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/lfg.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/nod_api.cpp"
    "${_wii_disc_root}/src/platform/wii_disc/rvz.cpp"
)
target_include_directories(charged_wii_disc PUBLIC "${_wii_disc_root}/src/platform/wii_disc/include")
target_compile_features(charged_wii_disc PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(charged_wii_disc PUBLIC Threads::Threads)
target_link_libraries(charged_wii_disc PRIVATE ${_wii_disc_zstd} BZip2::BZip2 LibLZMA::LibLZMA)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    # The test writes its RVZ and WIA images with the same codecs.
    add_executable(wii_disc_reader_tests "${_wii_disc_root}/tests/wii_disc_reader.cpp")
    target_include_directories(wii_disc_reader_tests PRIVATE "${_wii_disc_root}/src")
    target_compile_features(wii_disc_reader_tests PRIVATE cxx_std_17)
    target_link_libraries(wii_disc_reader_tests PRIVATE charged_wii_disc ${_wii_disc_zstd} BZip2::BZip2
        LibLZMA::LibLZMA)
    set(_wii_disc_header_check)
    if(MSCHARGED_NOD_PREPARED)
        set(_wii_disc_header_check "${_wii_disc_root}/src/platform/wii_disc/include/nod.h"
            "${MSCHARGED_NOD_PREPARED}/nod-ffi/include/nod.h")
    endif()
    add_test(NAME wii_disc_reader
        COMMAND wii_disc_reader_tests "${CMAKE_CURRENT_BINARY_DIR}/wii_disc_reader" ${_wii_disc_header_check})
    set_tests_properties(wii_disc_reader PROPERTIES TIMEOUT 300)
endif()
