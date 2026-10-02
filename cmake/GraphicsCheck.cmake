add_executable(mscharged-gx-check src/runtime/gx_check.cpp)
target_include_directories(mscharged-gx-check PRIVATE src)
target_compile_features(mscharged-gx-check PRIVATE cxx_std_20)
target_link_libraries(mscharged-gx-check PRIVATE aurora::gx aurora::mtx aurora::os
    aurora::vi aurora::core mscharged_build_info)

# Real GPU tests require a desktop and driver. Keep the portable tests runnable
# without a GPU; register this explicit gate only when requested.
option(MSCHARGED_TEST_VULKAN "Run the GX diagnostic through a real Vulkan device in CTest" OFF)
if(BUILD_TESTING)
    add_test(NAME sqlite_preparation
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_sqlite.py")
endif()
if(BUILD_TESTING AND MSCHARGED_TEST_VULKAN)
    add_test(NAME gx_vulkan COMMAND mscharged-gx-check --frames 180 --resize-test)
    add_test(NAME gx_vulkan_optimized COMMAND mscharged-gx-check --frames 180 --optimized-device)
    # Require the installed layer even if Dawn would otherwise skip an absent one.
    set_tests_properties(gx_vulkan gx_vulkan_optimized PROPERTIES TIMEOUT 45 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Error:|Validation Error|GX check failed")
endif()
