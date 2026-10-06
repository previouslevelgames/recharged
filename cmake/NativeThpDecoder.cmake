include_guard(GLOBAL)
# Actual prepared Aurora CPU decoder; this provider lives in the host's one SDK
# foundation. Its MIT terms remain applicable; no game/movie policy is supplied.
add_library(charged_thp_decoder STATIC
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/thp/THPDec.cpp"
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/thp/THPAudio.cpp")
add_dependencies(charged_thp_decoder verify_prepared)
target_link_libraries(charged_thp_decoder PUBLIC aurora::core)
target_compile_features(charged_thp_decoder PUBLIC cxx_std_20)
target_compile_definitions(charged_thp_decoder PRIVATE AURORA_THP_PRESERVE_QUARTER_IDCT=1
    AURORA_THP_HARDWARE_SAFE_POINTS=1)
