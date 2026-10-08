include_guard(GLOBAL)
include(cmake/OriginalHBMWii16.cmake)
include(cmake/NativeHBMDebug.cmake)

# Representation only: the caller separately selects the genuine original
# Layout provider closure. This helper does not create a HOME owner or allocator.
# It must share the sole original module's ARC and allocation registry.
function(mscharged_select_original_hbm_layout_transport target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Original HBM layout transport needs its actual module")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HBM layout transport belongs to the sole original module")
    endif()
    get_target_property(_selected "${target}" SOURCES)
    set(_source "${PROJECT_SOURCE_DIR}/src/platform/native_hbm_layout.cpp")
    file(RELATIVE_PATH _relative "${PROJECT_SOURCE_DIR}" "${_source}")
    if(NOT _source IN_LIST _selected AND NOT _relative IN_LIST _selected)
        target_sources("${target}" PRIVATE "${_source}")
        # The typed resource records share the original library's Wii16 ABI.
        set_property(SOURCE "${_source}" TARGET_DIRECTORY "${target}"
            APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
    endif()
    target_compile_features("${target}" PRIVATE cxx_std_20)
    target_include_directories("${target}" PRIVATE "${PROJECT_SOURCE_DIR}/src")
endfunction()

if(BUILD_TESTING)
    add_executable(native_hbm_layout_tests tests/native_hbm_layout.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/lyt/lyt_common.cpp"
        "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
        src/platform/native_hbm_layout.cpp src/platform/arc_data_transport.cpp
        src/platform/game_allocation_ownership.cpp src/platform/host_metadata.cpp)
    # Generated serialized bytes exercise representation and the original header
    # predicate. The test does not install a Layout allocator or create HOME.
    mscharged_select_original_hbm_debug(native_hbm_layout_tests)
    mscharged_link_original_hbm_debug_host(native_hbm_layout_tests CPU_FIXTURE)
    add_dependencies(native_hbm_layout_tests verify_prepared)
    target_compile_features(native_hbm_layout_tests PRIVATE cxx_std_20)
    target_include_directories(native_hbm_layout_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/include"
        "${MSCHARGED_PREPARED}" "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_hbm_layout_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 HBM_ASSERT=1)
    target_compile_options(native_hbm_layout_tests PRIVATE -fshort-wchar -fno-rtti
        -fno-strict-aliasing -ffp-contract=off -ffunction-sections -fdata-sections
        -Wno-unknown-pragmas)
    if(APPLE)
        target_link_options(native_hbm_layout_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(native_hbm_layout_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME native_hbm_layout COMMAND native_hbm_layout_tests)
    set_tests_properties(native_hbm_layout PROPERTIES TIMEOUT 15 LABELS "Platform")
endif()
