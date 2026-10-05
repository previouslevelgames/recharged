# Whole reconstructed registry units, independent of the historical scoped
# wrapper. This compiler/provider gate does not establish full game startup.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_library(charged_platform_thread STATIC src/platform/thread.cpp)
target_include_directories(charged_platform_thread PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(charged_platform_thread PUBLIC cxx_std_17)
target_link_libraries(charged_platform_thread PUBLIC Threads::Threads)

add_library(charged_original_tweak_value_base OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/TweakValueBase.cpp")
add_library(charged_original_tweaks OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/TweakRegistry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNode.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakEntry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValue.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakConfig.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNameRecycler.cpp")
foreach(target IN ITEMS charged_original_tweak_value_base charged_original_tweaks)
    add_dependencies(${target} verify_prepared)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(${target} PRIVATE MSCHARGED_NATIVE=1)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
            -ffunction-sections -fdata-sections)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /Gy /Gw)
    endif()
endforeach()
add_custom_target(charged_tweaks_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_tweaks>;$<TARGET_OBJECTS:charged_original_tweak_value_base>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-tweaks-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_tweaks charged_original_tweak_value_base
    COMMENT "Record original registry references; whole Game/AIPad providers remain required"
    VERBATIM)

if(BUILD_TESTING)
    add_executable(original_tweak_stack_tests tests/original_tweak_stack.cpp
        $<TARGET_OBJECTS:charged_original_tweak_value_base>)
    target_compile_features(original_tweak_stack_tests PRIVATE cxx_std_17)
    target_link_libraries(original_tweak_stack_tests PRIVATE charged_platform_thread)
    # Only the real original stack classification is executed. Uncalled base
    # constructors/destructors still require the original complete registry.
    if(MSVC)
        target_link_options(original_tweak_stack_tests PRIVATE /OPT:REF)
    elseif(APPLE)
        target_link_options(original_tweak_stack_tests PRIVATE -Wl,-dead_strip)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(original_tweak_stack_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME original_tweak_stack COMMAND original_tweak_stack_tests)
    set_tests_properties(original_tweak_stack PROPERTIES TIMEOUT 30)
    add_executable(original_tweak_storage_tests tests/original_tweak_storage.cpp)
    target_compile_features(original_tweak_storage_tests PRIVATE cxx_std_17)
    target_include_directories(original_tweak_storage_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_tweak_storage_tests PRIVATE charged_decomp_startup)
    add_test(NAME original_tweak_storage COMMAND original_tweak_storage_tests)
    set_tests_properties(original_tweak_storage PROPERTIES TIMEOUT 30)
endif()
