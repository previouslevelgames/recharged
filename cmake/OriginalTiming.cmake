# Original timing/debug units; kept separate from selected-source diagnostics
# until their full game and rendering providers are linked.
include_guard(GLOBAL)
add_library(charged_original_timing OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Debug/FrameCounter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Debug/Histogram.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Debug/TimeRegions.cpp")
add_dependencies(charged_original_timing verify_prepared)
target_include_directories(charged_original_timing PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_timing PRIVATE MSCHARGED_NATIVE=1 dSINGLE=1)
target_compile_features(charged_original_timing PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_timing PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
add_custom_target(charged_timing_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_timing>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-timing-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_timing
    COMMENT "Record unresolved references from whole original timing units (not a link check)"
    VERBATIM)
