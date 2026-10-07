include_guard(GLOBAL)

# Original SaveLoad alone consumes Wii16 wchar text and fixed32 TPL records.
# This does not initialize a banner, choose icons, supply CARD/NAND readiness,
# or qualify the full original title/save flow.
function(mscharged_add_original_save_icon_transport target)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
        message(FATAL_ERROR "Original SaveLoad Wii16 transport requires the qualified GNU/Clang wchar ABI")
    endif()
    get_target_property(_charged_has_icon_transport ${target}
        MSCHARGED_ORIGINAL_ICON_TRANSPORT)
    if(_charged_has_icon_transport)
        return()
    endif()
    include(cmake/WiiStringFormat.cmake)
    target_sources(${target} PRIVATE src/platform/native_tpl.cpp)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE charged_wii_msl)
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/Game/DB/SaveLoad.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
    set_property(TARGET ${target} PROPERTY MSCHARGED_ORIGINAL_ICON_TRANSPORT ON)
endfunction()
