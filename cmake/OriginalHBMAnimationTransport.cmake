include_guard(GLOBAL)
include(cmake/OriginalHBMWii16.cmake)
include(cmake/NativeHBMDebug.cmake)

# Representation beneath unchanged original Layout/animation algorithms.
# Call only on the sole original module with its genuine Layout provider closure.
# This helper does not create a HOME owner or install a Layout allocator.
function(mscharged_select_original_hbm_animation_transport target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Original HBM animation transport needs the actual module")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HBM animation transport belongs to the sole original module")
    endif()
    get_target_property(_selected "${target}" SOURCES)
    foreach(_source IN ITEMS
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/lyt/lyt_layout.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/lyt/lyt_animation.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/lyt/lyt_common.cpp"
        "${PROJECT_SOURCE_DIR}/src/platform/native_hbm_animation.cpp")
        list(FIND _selected "${_source}" _already)
        if(_already EQUAL -1)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _selected "${_source}")
        endif()
    endforeach()
endfunction()

if(BUILD_TESTING)
    add_executable(native_hbm_animation_tests tests/native_hbm_animation.cpp
        tests/diagnostics/original_hbm_animation_curves.cpp
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/lyt/lyt_common.cpp"
        "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
        src/platform/native_hbm_animation.cpp src/platform/arc_data_transport.cpp
        src/platform/game_allocation_ownership.cpp src/platform/host_metadata.cpp)
    # The test adapter includes the COMPLETE original lyt_animation.cpp to expose
    # its anonymous pure curve routines. Do not also compile that TU separately.
    mscharged_select_original_hbm_debug(native_hbm_animation_tests)
    mscharged_link_original_hbm_debug_host(native_hbm_animation_tests CPU_FIXTURE)
    add_dependencies(native_hbm_animation_tests verify_prepared)
    target_compile_features(native_hbm_animation_tests PRIVATE cxx_std_20)
    target_include_directories(native_hbm_animation_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/include"
        "${MSCHARGED_PREPARED}" "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(native_hbm_animation_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1 HBM_ASSERT=1)
    target_compile_options(native_hbm_animation_tests PRIVATE -fshort-wchar -fno-rtti
        -fno-strict-aliasing -ffp-contract=off -ffunction-sections -fdata-sections
        -Wno-unknown-pragmas)
    if(APPLE)
        target_link_options(native_hbm_animation_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(native_hbm_animation_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME native_hbm_animation COMMAND native_hbm_animation_tests)
    set_tests_properties(native_hbm_animation PROPERTIES TIMEOUT 15 LABELS "Platform")
endif()
