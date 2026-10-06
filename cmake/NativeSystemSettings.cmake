include_guard(GLOBAL)
find_package(Threads REQUIRED)

# Explicit virtual Wii system records beneath original SC requests. Staging
# precedes source module constructors; source SCInit publishes actual readiness.
add_library(charged_native_system_settings STATIC src/platform/system.cpp)
add_dependencies(charged_native_system_settings verify_prepared)
target_compile_features(charged_native_system_settings PUBLIC cxx_std_17)
target_compile_definitions(charged_native_system_settings PUBLIC
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_include_directories(charged_native_system_settings PUBLIC src
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_native_system_settings PUBLIC Threads::Threads)

if(BUILD_TESTING)
    add_executable(native_system_settings_tests tests/native_system_settings.cpp)
    target_link_libraries(native_system_settings_tests PRIVATE charged_native_system_settings)
    add_test(NAME native_system_settings COMMAND native_system_settings_tests)
    set_tests_properties(native_system_settings PROPERTIES TIMEOUT 30)
endif()
