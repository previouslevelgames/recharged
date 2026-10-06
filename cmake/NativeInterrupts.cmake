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
