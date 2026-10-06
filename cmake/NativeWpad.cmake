include_guard(GLOBAL)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NativeInterrupts.cmake")

# Native hardware SDK interfaces underneath the original game/KPAD code.
# Owner servicing is explicit; no alternate pad manager or device readiness.
add_library(charged_native_wpad STATIC EXCLUDE_FROM_ALL
    src/platform/wpad_sdl.cpp src/platform/os_version.cpp)
add_dependencies(charged_native_wpad verify_prepared)
target_include_directories(charged_native_wpad PUBLIC src PRIVATE
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_features(charged_native_wpad PRIVATE cxx_std_20)
target_compile_definitions(charged_native_wpad PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1)
target_link_libraries(charged_native_wpad PUBLIC
    charged_native_interrupts SDL3::SDL3 aurora::os)
