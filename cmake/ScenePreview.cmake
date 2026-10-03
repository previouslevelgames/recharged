include(cmake/NativeRuntime.cmake)
include(cmake/FrameTiming.cmake)
add_library(charged_compressed_assets STATIC src/resources/compressed_asset.cpp)
target_include_directories(charged_compressed_assets PUBLIC src)
target_compile_features(charged_compressed_assets PUBLIC cxx_std_20)
target_link_libraries(charged_compressed_assets PRIVATE ZLIB::ZLIB)
if(BUILD_TESTING)
    add_executable(compressed_asset_tests tests/compressed_assets.cpp)
    target_link_libraries(compressed_asset_tests PRIVATE charged_compressed_assets)
    add_test(NAME compressed_assets COMMAND compressed_asset_tests)
endif()
add_library(charged_materials STATIC
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialAlphaState.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GameObjectLightingCore.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/LightingLookupNative.cpp"
    src/runtime/materials.cpp src/runtime/material_gx.cpp src/runtime/material_texture.cpp
    src/runtime/gpu_readback.cpp src/runtime/lighting.cpp)
foreach(program UnlitTexture VertexColourTexture ScrollingDiffuse MaskedSpecularFresnel ShadowVolume SpecularDetailBlend ScrollingSpecular CameraScrolledOverlay)
    target_sources(charged_materials PRIVATE
        "${MSCHARGED_PREPARED}/src/NL/glx/GX${program}MaterialProgram.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/GX${program}MaterialProgramRender.cpp")
endforeach()
add_dependencies(charged_materials verify_prepared)
target_include_directories(charged_materials PUBLIC src PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_features(charged_materials PUBLIC cxx_std_20)
target_compile_definitions(charged_materials PRIVATE TARGET_PC=1)
target_link_libraries(charged_materials PUBLIC charged_graphics_memory charged_static_resources PRIVATE aurora::gx aurora::mtx)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_materials PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
add_library(charged_frames STATIC
    "${MSCHARGED_PREPARED}/src/NL/gl/glFrame.cpp"
    src/runtime/frames.cpp src/runtime/frame_aurora.cpp)
add_dependencies(charged_frames verify_prepared)
target_include_directories(charged_frames PUBLIC src PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_frames PUBLIC charged_views PRIVATE aurora::gx)
target_compile_features(charged_frames PUBLIC cxx_std_20)
add_library(charged_static_inventory STATIC src/runtime/static_inventory.cpp)
target_compile_features(charged_static_inventory PUBLIC cxx_std_20)
target_include_directories(charged_static_inventory PUBLIC src)
target_link_libraries(charged_static_inventory PUBLIC charged_materials)
add_library(charged_views STATIC
    "${MSCHARGED_PREPARED}/src/NL/gl/glView.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glRenderList.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTarget.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glStruct.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glViewMath.cpp"
    src/runtime/views.cpp src/runtime/targets.cpp)
add_dependencies(charged_views verify_prepared)
target_include_directories(charged_views PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_views PUBLIC charged_materials PRIVATE aurora::gx)
target_compile_features(charged_views PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_views PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
if(BUILD_TESTING)
    add_executable(render_views_tests tests/render_views.cpp)
    target_link_libraries(render_views_tests PRIVATE charged_views)
    add_test(NAME render_views COMMAND render_views_tests)
    set_tests_properties(render_views PROPERTIES TIMEOUT 30)
    add_executable(static_inventory_tests tests/static_inventory.cpp)
    target_link_libraries(static_inventory_tests PRIVATE charged_static_inventory)
    add_test(NAME static_inventory COMMAND static_inventory_tests)
    set_tests_properties(static_inventory PROPERTIES TIMEOUT 30)
endif()
add_library(charged_shadows STATIC
    "${MSCHARGED_PREPARED}/src/NL/gl/glModelCopies.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxModelClone.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/StadiumShadowNative.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/glModelBuilder.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLShadowBlendMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/ShadowVolume.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/RLView.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/ShadowLayersNative.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/RenderShadowNative.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/Frustum.cpp"
    src/runtime/shadows.cpp)
add_dependencies(charged_shadows verify_prepared)
target_include_directories(charged_shadows PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_shadows PUBLIC charged_views PRIVATE aurora::gx)
target_compile_features(charged_shadows PUBLIC cxx_std_20)
if(BUILD_TESTING)
    add_executable(shadows_tests tests/shadows.cpp)
    target_link_libraries(shadows_tests PRIVATE charged_shadows)
    add_test(NAME shadows COMMAND shadows_tests)
    set_tests_properties(shadows PROPERTIES TIMEOUT 30)
    add_executable(shadow_pipeline_tests tests/shadow_pipeline.cpp)
    target_link_libraries(shadow_pipeline_tests PRIVATE charged_shadows charged_static_inventory aurora::gx aurora::vi aurora::core)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_shadows PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
add_library(charged_scene_preview STATIC src/runtime/scene.cpp)
target_compile_features(charged_scene_preview PRIVATE cxx_std_20)
target_include_directories(charged_scene_preview PUBLIC src)
target_link_libraries(charged_scene_preview PRIVATE charged_frames charged_frame_timing charged_shadows charged_compressed_assets charged_static_inventory charged_decomp_startup
    charged_host aurora::gx aurora::mtx aurora::os aurora::vi aurora::dvd aurora::core mscharged_build_info)
target_link_libraries(mscharged PRIVATE charged_scene_preview)
target_compile_definitions(mscharged PRIVATE MSCHARGED_HAS_SCENE_PREVIEW=1)
if(BUILD_TESTING)
    include(cmake/Tasks.cmake)
    add_executable(graphics_frames_tests tests/graphics_frames.cpp tests/task_clock.cpp)
    target_include_directories(graphics_frames_tests PRIVATE "${MSCHARGED_PREPARED}/src")
    target_link_libraries(graphics_frames_tests PRIVATE charged_frames charged_tasks)
    add_test(NAME graphics_frames COMMAND graphics_frames_tests)
    set_tests_properties(graphics_frames PROPERTIES TIMEOUT 30)
    add_executable(frame_pipeline_tests tests/frame_pipeline.cpp)
    target_link_libraries(frame_pipeline_tests PRIVATE charged_frames charged_static_inventory aurora::gx aurora::vi aurora::core)
    add_executable(material_pipeline_tests tests/material_pipeline.cpp)
    target_link_libraries(material_pipeline_tests PRIVATE charged_views charged_static_inventory aurora::gx aurora::vi aurora::core)
endif()
if(BUILD_TESTING AND MSCHARGED_TEST_VULKAN)
    add_test(NAME frame_pipeline COMMAND frame_pipeline_tests)
    set_tests_properties(frame_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME shadow_pipeline COMMAND shadow_pipeline_tests)
    set_tests_properties(shadow_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME material_pipeline COMMAND material_pipeline_tests)
    set_tests_properties(material_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME specular_detail_pipeline COMMAND material_pipeline_tests --specular-only)
    set_tests_properties(specular_detail_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME scrolling_specular_pipeline COMMAND material_pipeline_tests --scrolling-specular-only)
    set_tests_properties(scrolling_specular_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME scene_synthetic
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scene.py" "$<TARGET_FILE:mscharged>")
    add_test(NAME camera_overlay_pipeline COMMAND material_pipeline_tests --camera-overlay-only)
    set_tests_properties(camera_overlay_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    set_tests_properties(scene_synthetic PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
endif()
