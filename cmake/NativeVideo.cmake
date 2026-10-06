include_guard(GLOBAL)
add_library(charged_native_video_device STATIC src/platform/video_device.cpp)
add_dependencies(charged_native_video_device verify_prepared)
target_include_directories(charged_native_video_device PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_video_device PUBLIC TARGET_PC=1)
target_compile_features(charged_native_video_device PUBLIC cxx_std_17)
target_link_libraries(charged_native_video_device PUBLIC
    charged_native_interrupt_controller aurora::vi)
