include_guard(GLOBAL)
# Host hardware foundation for the named same-module original Credits movie
# diagnostic. The source scope and whole THPSimple/movie units are selected by
# OriginalCreditsDiagnostic; this file supplies no alternate movie/game flow.
include(cmake/NativeAI.cmake)
include(cmake/NativeThpDecoder.cmake)
add_library(charged_credits_movie_hardware STATIC
    tests/diagnostics/credits_movie_hardware.cpp)
add_dependencies(charged_credits_movie_hardware verify_prepared)
target_compile_features(charged_credits_movie_hardware PUBLIC cxx_std_20)
target_include_directories(charged_credits_movie_hardware PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics")
target_link_libraries(charged_credits_movie_hardware PUBLIC
    charged_native_ai charged_thp_decoder)
# Whole original THPSimple is hidden inside the existing game module. Its
# actual compiler/decoder/AI imports resolve to this one host SDK foundation.
target_link_options(charged_credits_movie_hardware INTERFACE
    -Wl,--undefined=AIInit -Wl,--undefined=THPInit
    -Wl,--undefined=THPVideoDecode -Wl,--undefined=THPAudioDecode)

# The existing movie SDK owner composes optional input beneath original APIs.
include(cmake/NativeHardwareOwner.cmake)
target_link_libraries(charged_credits_movie_hardware PUBLIC charged_native_hardware_owner)
