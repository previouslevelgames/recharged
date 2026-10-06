include_guard(GLOBAL)
# Opt in only for real GX/native-VI owners. CPU-only hardware diagnostics retain
# NativeVideo.cmake without acquiring a graphics endpoint or surface dependency.
include(cmake/NativeVideo.cmake)
add_library(charged_native_video_output_device STATIC
    src/platform/video_output_device.cpp)
add_dependencies(charged_native_video_output_device verify_prepared)
target_include_directories(charged_native_video_output_device PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_video_output_device PUBLIC TARGET_PC=1)
target_compile_features(charged_native_video_output_device PUBLIC cxx_std_17)
target_link_libraries(charged_native_video_output_device PUBLIC
    charged_native_video_device aurora::gx)
