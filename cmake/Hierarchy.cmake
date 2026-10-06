include_guard(GLOBAL)
add_library(charged_hierarchy_reader STATIC src/resources/hierarchy.cpp)
target_include_directories(charged_hierarchy_reader PUBLIC src)
target_compile_features(charged_hierarchy_reader PUBLIC cxx_std_20)
add_library(charged_hierarchy_assets STATIC
    "${MSCHARGED_PREPARED}/src/Game/SHierarchy.cpp"
    src/runtime/hierarchy_assets.cpp)
add_dependencies(charged_hierarchy_assets verify_prepared)
target_compile_definitions(charged_hierarchy_assets PRIVATE MSCHARGED_DIAGNOSTIC_SKELETON=1)
target_include_directories(charged_hierarchy_assets PUBLIC
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_hierarchy_assets PUBLIC MSCHARGED_NATIVE=1)
target_link_libraries(charged_hierarchy_assets PUBLIC charged_hierarchy_reader)
if(BUILD_TESTING)
    add_executable(hierarchy_tests tests/hierarchy.cpp)
    target_link_libraries(hierarchy_tests PRIVATE charged_hierarchy_assets)
    add_test(NAME hierarchy COMMAND hierarchy_tests)
    set_tests_properties(hierarchy PROPERTIES TIMEOUT 30)
    add_test(NAME hierarchy_oracle
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hierarchy.py"
            "$<TARGET_FILE:hierarchy_tests>")
    set_tests_properties(hierarchy_oracle PROPERTIES TIMEOUT 30)
endif()
