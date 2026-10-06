include_guard(GLOBAL)
find_package(Threads REQUIRED)
# Shared native SDK interrupt/context services. Device workers latch hardware
# completion; source callbacks run on the owner under this exclusion boundary.
add_library(charged_native_interrupts STATIC src/platform/interrupts.cpp)
add_dependencies(charged_native_interrupts verify_prepared)
target_include_directories(charged_native_interrupts PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_interrupts PUBLIC TARGET_PC=1)
target_compile_features(charged_native_interrupts PUBLIC cxx_std_17)
target_link_libraries(charged_native_interrupts PUBLIC Threads::Threads)

# Native hardware line registration/masks and owner-thread delivery. Device
# endpoints assert real levels; this provider supplies no DSP/mixer readiness.
add_library(charged_native_interrupt_controller STATIC src/platform/interrupt_controller.cpp)
add_dependencies(charged_native_interrupt_controller verify_prepared)
target_include_directories(charged_native_interrupt_controller PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_interrupt_controller PUBLIC TARGET_PC=1)
target_compile_features(charged_native_interrupt_controller PUBLIC cxx_std_17)
target_link_libraries(charged_native_interrupt_controller PUBLIC charged_native_interrupts
    PRIVATE aurora::os)

if(BUILD_TESTING)
    add_executable(native_interrupt_controller_tests tests/native_interrupt_controller.cpp)
    target_link_libraries(native_interrupt_controller_tests PRIVATE charged_native_interrupt_controller)
    target_compile_features(native_interrupt_controller_tests PRIVATE cxx_std_17)
    add_test(NAME native_interrupt_controller COMMAND native_interrupt_controller_tests)
    set_tests_properties(native_interrupt_controller PROPERTIES TIMEOUT 30)
endif()
