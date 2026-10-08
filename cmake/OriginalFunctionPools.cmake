include_guard(GLOBAL)
get_filename_component(_function_pool_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Pure native ABI usage, without a second memory/provider/SDK instance. Every
# original TU in the future isolated game module must consume this profile;
# mixing it with legacy native-size callback routing is not a production graph.
add_library(charged_original_function_pool_abi INTERFACE)
target_compile_definitions(charged_original_function_pool_abi INTERFACE
    MSCHARGED_NATIVE=1 MSCHARGED_ORIGINAL_FUNCTION_POOLS=1 TARGET_PC=1)
target_include_directories(charged_original_function_pool_abi INTERFACE
    "${_function_pool_root}/src"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_PREPARED}/libs/Runtime/include")
target_compile_features(charged_original_function_pool_abi INTERFACE cxx_std_17)
# Match the original MWCC -RTTI off compiler profile across every original TU.
# Retain virtual methods/vtables; host-side exception reporting remains enabled.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_function_pool_abi INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
endif()

# Declared callable fields and independently authored Wii/native footprints.
# Unused original virtual bodies are collected solely in this layout qualifier;
# it does not construct original game/audio/events or qualify their readiness.
# Actual whole-source pool/Clone/deletion was separately exercised against one
# SDK foundation with original allocation/SlotPool stack providers.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(original_function_layout_tests "${_function_pool_root}/tests/original_function_layout.cpp")
    if(TARGET verify_prepared)
        add_dependencies(original_function_layout_tests verify_prepared)
    endif()
    target_link_libraries(original_function_layout_tests PRIVATE
        charged_original_function_pool_abi)
    target_compile_definitions(original_function_layout_tests PRIVATE MSCHARGED_GAME_MODULE=1)
    target_include_directories(original_function_layout_tests PRIVATE
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_options(original_function_layout_tests PRIVATE
        -ffunction-sections -fdata-sections -fno-strict-aliasing
        -Wno-unknown-pragmas -Wno-invalid-offsetof)
    target_link_options(original_function_layout_tests PRIVATE -Wl,--gc-sections)
    if(MINGW)
        target_link_options(original_function_layout_tests PRIVATE -static)
    endif()
    add_test(NAME original_function_layout COMMAND original_function_layout_tests)
    set_tests_properties(original_function_layout PROPERTIES TIMEOUT 30)
endif()
