include_guard(GLOBAL)
# Original MSL consumes the game's Wii16 strings. Isolate its libc symbols and
# wchar ABI from host dependencies; do not pass these strings to host vswprintf.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    message(FATAL_ERROR "The original Wii16 formatter currently requires a GNU/Clang wchar ABI qualifier")
endif()
set(_charged_wii_print_sources
    "${MSCHARGED_PREPARED}/src/MSL/wprintf.c"
    "${MSCHARGED_PREPARED}/src/MSL/wstring.c"
    "${MSCHARGED_PREPARED}/src/MSL/wmem.c"
    "${MSCHARGED_PREPARED}/src/MSL/mbstring.c"
    "${MSCHARGED_PREPARED}/src/MSL/locale.c"
    "${MSCHARGED_PREPARED}/src/MSL/wctype.c"
    "${MSCHARGED_PREPARED}/src/MSL/ansi_fp.c"
    "${MSCHARGED_PREPARED}/src/MSL/math_api.c"
    "${MSCHARGED_PREPARED}/src/MSL/ctype.c"
    "${MSCHARGED_PREPARED}/src/MSL/float.c"
)
set_source_files_properties(${_charged_wii_print_sources} PROPERTIES LANGUAGE CXX)
add_library(charged_wii_msl STATIC ${_charged_wii_print_sources})
add_dependencies(charged_wii_msl verify_prepared)
target_compile_features(charged_wii_msl PRIVATE cxx_std_20)
target_include_directories(charged_wii_msl PRIVATE
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_PREPARED}/libs/MSL_C/include")
target_compile_options(charged_wii_msl PRIVATE
    -fshort-wchar -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
target_compile_definitions(charged_wii_msl PRIVATE MSCHARGED_NATIVE=1)
if(CMAKE_CXX_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
    target_compile_definitions(charged_wii_msl PRIVATE __LITTLE_ENDIAN__=1)
else()
    target_compile_definitions(charged_wii_msl PRIVATE __BIG_ENDIAN__=1)
endif()
foreach(symbol IN ITEMS
    vswprintf __wpformatter __wStringWrite wcscpy wcsncpy
    wcslen wcschr wcscmp wmemchr wmemcpy
    mbtowc wctomb mbstowcs wcstombs __mbtowc_noconv
    __wctomb_noconv __wctype_map _current_locale __num2dec __num2dec_internal
    __ull2dec __two_exp __timesdec __str2dec __equals_dec
    __less_dec __minus_dec __dec2num __lconv __ctype_cmpt
    __coll_cmpt __mon_cmpt __num_cmpt __time_cmpt char_coll_table
    __ctype_map __upper_map __lower_map __wupper_map __wlower_map
    __fpclassifyf __fpclassifyd __signbitd __float_huge __float_nan
    __double_huge isalpha isdigit isspace isupper
    isxdigit tolower toupper iswdigit iswupper
)
    target_compile_definitions(charged_wii_msl PRIVATE "${symbol}=ChargedWii_${symbol}")
endforeach()
add_library(charged_wii_string_format STATIC src/platform/wii_string_format.cpp)
target_compile_features(charged_wii_string_format PRIVATE cxx_std_17)
target_include_directories(charged_wii_string_format PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(charged_wii_string_format PRIVATE -fshort-wchar -fno-strict-aliasing)
target_link_libraries(charged_wii_string_format PRIVATE charged_wii_msl)
