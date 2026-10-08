include_guard(GLOBAL)
include(cmake/OriginalHBMWii16.cmake)
include(cmake/NativeHBMDebug.cmake)

# Requires the existing sole game ownership registry, exact ARC borrowed-file
# transport, Wii16 HBM compiler profile and original assertion/debug providers.
# Selects representation beneath original ResFont; admits no HomeButton UI flow.
function(mscharged_select_original_hbm_font_transport target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Original HBM font transport needs its actual module target")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HBM font transport belongs to the original module")
    endif()
    get_target_property(_selected "${target}" SOURCES)
    set(_font_sources
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/ut_ResFont.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/ut_ResFontBase.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/ut_Font.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/ut_CharStrmReader.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/ut_binaryFileFormat.cpp"
        "${PROJECT_SOURCE_DIR}/src/platform/native_hbm_font.cpp")
    foreach(_source IN LISTS _font_sources)
        list(FIND _selected "${_source}" _already)
        if(_already EQUAL -1)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _selected "${_source}")
        endif()
    endforeach()
endfunction()

if(BUILD_TESTING)
    add_executable(native_hbm_font_tests tests/native_hbm_font.cpp
        "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
        src/platform/native_hbm_font.cpp src/platform/arc_data_transport.cpp
        src/platform/game_allocation_ownership.cpp src/platform/host_metadata.cpp)
    foreach(_name IN ITEMS ut_ResFont ut_ResFontBase ut_Font ut_CharStrmReader ut_binaryFileFormat)
        target_sources(native_hbm_font_tests PRIVATE
            "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/ut/${_name}.cpp")
    endforeach()
    mscharged_select_original_hbm_debug(native_hbm_font_tests)
    mscharged_link_original_hbm_debug_host(native_hbm_font_tests CPU_FIXTURE)
    add_dependencies(native_hbm_font_tests verify_prepared)
    target_compile_features(native_hbm_font_tests PRIVATE cxx_std_20)
    target_include_directories(native_hbm_font_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/include"
        "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_hbm_font_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 HBM_ASSERT=1)
    target_compile_options(native_hbm_font_tests PRIVATE -fshort-wchar -fno-rtti
        -fno-strict-aliasing -ffp-contract=off -ffunction-sections -fdata-sections
        -Wno-unknown-pragmas)
    if(APPLE)
        target_link_options(native_hbm_font_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(native_hbm_font_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME native_hbm_font COMMAND native_hbm_font_tests)
    set_tests_properties(native_hbm_font PROPERTIES TIMEOUT 15 LABELS "Platform")
endif()
