include_guard(GLOBAL)
include(cmake/NativeHBMDebug.cmake)

if(BUILD_TESTING)
    add_executable(native_hbm_color_tests tests/native_hbm_color.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/lyt/lyt_common.cpp")
    mscharged_select_original_hbm_debug(native_hbm_color_tests)
    mscharged_link_original_hbm_debug_host(native_hbm_color_tests CPU_FIXTURE)
    add_dependencies(native_hbm_color_tests verify_prepared)
    target_compile_features(native_hbm_color_tests PRIVATE cxx_std_20)
    target_include_directories(native_hbm_color_tests PRIVATE
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_hbm_color_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1 HBM_ASSERT=1)
    target_compile_options(native_hbm_color_tests PRIVATE -fshort-wchar -fno-rtti
        -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off
        -Wno-unknown-pragmas "$<$<CXX_COMPILER_ID:Clang>:-Wno-register>")
    if(APPLE)
        target_link_options(native_hbm_color_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(native_hbm_color_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME native_hbm_color COMMAND native_hbm_color_tests)
    set_tests_properties(native_hbm_color PROPERTIES TIMEOUT 10 LABELS "Platform")
endif()
