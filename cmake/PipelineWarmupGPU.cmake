include_guard(GLOBAL)
if(NOT BUILD_TESTING OR NOT TARGET aurora_gx)
    return()
endif()
add_executable(gpu_cache_retention_tests tests/gpu_cache_retention.cpp)
add_dependencies(gpu_cache_retention_tests verify_prepared)
target_include_directories(gpu_cache_retention_tests PRIVATE "${MSCHARGED_AURORA_PREPARED}/lib")
target_compile_features(gpu_cache_retention_tests PRIVATE cxx_std_20)
target_link_libraries(gpu_cache_retention_tests PRIVATE
    aurora::gx aurora::os aurora::vi aurora::core Threads::Threads sqlite3 absl::flat_hash_map)
add_test(NAME gpu_cache_retention COMMAND gpu_cache_retention_tests)
add_executable(pipeline_warmup_gpu_tests tests/pipeline_warmup_gpu.cpp)
add_dependencies(pipeline_warmup_gpu_tests verify_prepared)
target_include_directories(pipeline_warmup_gpu_tests PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/lib")
target_compile_features(pipeline_warmup_gpu_tests PRIVATE cxx_std_20)
target_link_libraries(pipeline_warmup_gpu_tests PRIVATE
    aurora::gx aurora::os aurora::vi aurora::core Threads::Threads sqlite3 absl::flat_hash_map)
if(MSCHARGED_TEST_VULKAN)
    add_test(NAME pipeline_warmup_gpu COMMAND pipeline_warmup_gpu_tests --backend vulkan)
    set_tests_properties(pipeline_warmup_gpu PROPERTIES TIMEOUT 180
        LABELS "gpu;vulkan" RESOURCE_LOCK gx_check
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        FAIL_REGULAR_EXPRESSION "VUID-|Error:|Validation Error|Pipeline warmup GPU failure")
endif()

# This target is portable even when Vulkan CTest execution is disabled (Windows).
add_executable(identity_texgen_gpu_tests tests/identity_texgen_gpu.cpp)
add_dependencies(identity_texgen_gpu_tests verify_prepared)
target_include_directories(identity_texgen_gpu_tests PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/lib")
target_compile_features(identity_texgen_gpu_tests PRIVATE cxx_std_20)
target_link_libraries(identity_texgen_gpu_tests PRIVATE
    aurora::gx aurora::mtx aurora::os aurora::vi aurora::core Threads::Threads sqlite3 absl::flat_hash_map)
if(MSCHARGED_TEST_VULKAN)
    add_test(NAME identity_texgen_gpu COMMAND identity_texgen_gpu_tests --backend vulkan)
    set_tests_properties(identity_texgen_gpu PROPERTIES TIMEOUT 180
        LABELS "gpu;vulkan" RESOURCE_LOCK gx_check
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        FAIL_REGULAR_EXPRESSION "VUID-|Error:|Validation Error|Identity texgen GPU failure")
endif()

# Fog and vertex array byte order are per-draw state (GX configuration v17).
add_executable(gx_draw_state_gpu_tests tests/gx_draw_state_gpu.cpp)
add_dependencies(gx_draw_state_gpu_tests verify_prepared)
target_include_directories(gx_draw_state_gpu_tests PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/lib")
target_compile_features(gx_draw_state_gpu_tests PRIVATE cxx_std_20)
target_link_libraries(gx_draw_state_gpu_tests PRIVATE
    aurora::gx aurora::mtx aurora::os aurora::vi aurora::core Threads::Threads sqlite3 absl::flat_hash_map)
if(MSCHARGED_TEST_VULKAN)
    add_test(NAME gx_draw_state_gpu COMMAND gx_draw_state_gpu_tests --backend vulkan)
    set_tests_properties(gx_draw_state_gpu PROPERTIES TIMEOUT 180
        LABELS "gpu;vulkan" RESOURCE_LOCK gx_check
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        FAIL_REGULAR_EXPRESSION "VUID-|Error:|Validation Error|GX draw state GPU failure")
endif()
