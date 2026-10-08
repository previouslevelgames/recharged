include_guard(GLOBAL)

# Real original allocator plus synthetic completed file bytes. This verifies
# representation/lifetime contracts, not HBM initialization or scene readiness.
add_executable(native_hbm_text_tests
    tests/native_hbm_text.cpp
    "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
    src/platform/game_allocation_ownership.cpp
    src/platform/hbm_text_transport.cpp
    src/platform/host_metadata.cpp)
add_dependencies(native_hbm_text_tests verify_prepared)
target_compile_features(native_hbm_text_tests PRIVATE cxx_std_20)
target_include_directories(native_hbm_text_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(native_hbm_text_tests PRIVATE
    MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1)
target_compile_options(native_hbm_text_tests PRIVATE
    -fshort-wchar -fno-strict-aliasing -ffp-contract=off -Wno-unknown-pragmas)
target_link_libraries(native_hbm_text_tests PRIVATE Threads::Threads charged_wii_msl)
add_test(NAME native_hbm_text COMMAND native_hbm_text_tests)
set_tests_properties(native_hbm_text PROPERTIES TIMEOUT 15 LABELS "Platform")
