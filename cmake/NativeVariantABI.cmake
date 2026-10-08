include_guard(GLOBAL)

if(BUILD_TESTING)
    # Intrinsic return words and the actual original union representation.
    # This does not instantiate the original class/string pool or AI cache.
    add_executable(variant_numeric_abi_tests tests/variant_numeric_abi.cpp)
    add_dependencies(variant_numeric_abi_tests verify_prepared)
    target_compile_features(variant_numeric_abi_tests PRIVATE cxx_std_20)
    target_include_directories(variant_numeric_abi_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(variant_numeric_abi_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 TARGET_PC=1)
    target_compile_options(variant_numeric_abi_tests PRIVATE -fno-rtti
        -fno-strict-aliasing -ffp-contract=off -fsigned-char
        -ffunction-sections -fdata-sections -Wno-unknown-pragmas)
    if(APPLE)
        target_link_options(variant_numeric_abi_tests PRIVATE "LINKER:-dead_strip")
    else()
        target_link_options(variant_numeric_abi_tests PRIVATE "LINKER:--gc-sections")
    endif()
    add_test(NAME variant_numeric_abi COMMAND variant_numeric_abi_tests)
    set_tests_properties(variant_numeric_abi PROPERTIES TIMEOUT 10 LABELS "Platform")
endif()
