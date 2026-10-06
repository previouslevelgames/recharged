include_guard(GLOBAL)

# Fixed Wii record fields beneath the complete original loader. This library
# supplies data/ABI transport only and has no source/game resource ownership.
add_library(charged_rlg_record_abi STATIC src/platform/rlg_record_abi.cpp)
add_dependencies(charged_rlg_record_abi verify_prepared)
target_compile_features(charged_rlg_record_abi PUBLIC cxx_std_20)
target_include_directories(charged_rlg_record_abi PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include")
target_compile_definitions(charged_rlg_record_abi PUBLIC MSCHARGED_NATIVE=1 TARGET_PC=1)

# This scan retains both complete original loader TUs. Raw chunk/parameter/
# index/matrix domains, actual pool lifetimes and GX array registration remain
# separate gates; it is not linked into an alternative source load path.
add_library(charged_original_rlg_loader OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/gl/glLoadModel.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxLoadModel.cpp")
add_dependencies(charged_original_rlg_loader verify_prepared)
target_compile_features(charged_original_rlg_loader PRIVATE cxx_std_20)
target_include_directories(charged_original_rlg_loader PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include" "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_rlg_loader PRIVATE charged_original_core charged_rlg_record_abi)
target_compile_definitions(charged_original_rlg_loader PRIVATE dSINGLE=1 __alloca=__builtin_alloca)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_rlg_loader PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(charged_original_rlg_loader PRIVATE -Wno-register)
    endif()
elseif(MSVC)
    target_compile_options(charged_original_rlg_loader PRIVATE /Gy /Gw)
endif()
add_custom_target(charged_rlg_loader_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_rlg_loader>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-rlg-loader-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_rlg_loader
    COMMENT "Compile whole original RLG loaders; raw Read/source lifetimes/GX remain pending"
    VERBATIM)

# The isolated CPU fixture uses the host CRT for test storage. It links only
# the real field adapter, with no original managers, statics or fake services.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
    AND CMAKE_C_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
    add_executable(original_rlg_record_tests tests/original_rlg_records.cpp)
    add_dependencies(original_rlg_record_tests verify_prepared)
    target_link_libraries(original_rlg_record_tests PRIVATE charged_rlg_record_abi)
    add_test(NAME original_rlg_records COMMAND original_rlg_record_tests)
    set_tests_properties(original_rlg_records PROPERTIES TIMEOUT 30
        PASS_REGULAR_EXPRESSION "original RLG record transport checks=")
endif()


# Native cache declarations share the actual SDK C ABI. This object is only a
# compiler inventory; it supplies no cache/GX implementation or runtime gate.
add_library(charged_original_cache_abi OBJECT EXCLUDE_FROM_ALL
    tests/original_cache_abi.cpp)
add_dependencies(charged_original_cache_abi verify_prepared)
target_compile_features(charged_original_cache_abi PRIVATE cxx_std_17)
target_include_directories(charged_original_cache_abi PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include" "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_cache_abi PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1)
add_dependencies(charged_rlg_loader_scan charged_original_cache_abi)
