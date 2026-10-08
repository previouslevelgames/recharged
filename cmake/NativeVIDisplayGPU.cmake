include_guard(GLOBAL)
if(NOT BUILD_TESTING OR NOT TARGET aurora_gx)
    return()
endif()
include(cmake/NativeVideoOutput.cmake)

# Use one target-local native VI provider, like the actual original main host.
# The normal aurora_vi archive stays ordinary and is never whole-linked here.
add_executable(native_vi_display_gpu_tests tests/native_vi_display_gpu.cpp
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/vi/vi.cpp")
add_dependencies(native_vi_display_gpu_tests verify_prepared)
target_include_directories(native_vi_display_gpu_tests PRIVATE src
    "${MSCHARGED_AURORA_PREPARED}/include"
    "${MSCHARGED_AURORA_PREPARED}/lib")
target_compile_features(native_vi_display_gpu_tests PRIVATE cxx_std_20)
target_compile_definitions(native_vi_display_gpu_tests PRIVATE
    AURORA_NATIVE_VIDEO=1 AURORA_WII_CLOCK=1 TARGET_PC=1)
target_link_libraries(native_vi_display_gpu_tests PRIVATE
    charged_native_video_output_device charged_native_video_device
    charged_native_interrupt_controller aurora::gx aurora::mtx
    aurora::os aurora::vi aurora::core Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(native_vi_display_gpu_tests PRIVATE -ffunction-sections -fdata-sections)
    if(APPLE)
        target_link_options(native_vi_display_gpu_tests PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(native_vi_display_gpu_tests PRIVATE -Wl,--gc-sections)
    endif()
endif()
# Always build the explicit hardware tool in a GX test profile. Register it
# only with the existing opt-in real-desktop GPU test policy.
if(MSCHARGED_TEST_VULKAN)
    add_test(NAME native_vi_display_gpu COMMAND native_vi_display_gpu_tests)
    set_tests_properties(native_vi_display_gpu PROPERTIES TIMEOUT 45
        LABELS "gpu;vulkan" RESOURCE_LOCK gx_check
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        FAIL_REGULAR_EXPRESSION "VUID-|Error:|Validation Error|Native VI display GPU failure")
endif()
