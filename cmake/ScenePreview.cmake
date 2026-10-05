include(cmake/NativeRuntime.cmake)
include(cmake/EffectsVertexRendering.cmake)
include(cmake/FrameTiming.cmake)
include(cmake/AnimatedCamera.cmake)
include(cmake/DebugCamera.cmake)
add_library(charged_debug_camera_input STATIC src/runtime/debug_camera_input.cpp)
target_link_libraries(charged_debug_camera_input PUBLIC charged_debug_camera SDL3::SDL3)
if(BUILD_TESTING)
    add_executable(debug_camera_input_tests tests/debug_camera_input.cpp tests/task_clock.cpp)
    target_include_directories(debug_camera_input_tests PRIVATE "${MSCHARGED_PREPARED}/src")
    target_link_libraries(debug_camera_input_tests PRIVATE charged_debug_camera_input)
    add_test(NAME debug_camera_input COMMAND debug_camera_input_tests)
    set_tests_properties(debug_camera_input PROPERTIES TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()
include(cmake/CompressedAssets.cmake)
add_library(charged_materials STATIC
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialAlphaState.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GameObjectLightingCore.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/LightingLookupNative.cpp"
    src/runtime/materials.cpp src/runtime/material_gx.cpp src/runtime/material_texture.cpp
    src/runtime/gpu_readback.cpp src/runtime/lighting.cpp src/runtime/skin_material.cpp
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSkinMatrix.cpp")
foreach(program UnlitTexture VertexColourTexture ScrollingDiffuse MaskedSpecularFresnel ShadowVolume SpecularDetailBlend ScrollingSpecular CameraScrolledOverlay MaskedDetailBlend ScrollingMaskedDetailBlend ScrollingCameraOverlay CharacterSkinCustom FloatTexturedColour ConstantColour)
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
    add_executable(texture_animation_tests tests/texture_animation.cpp)
    target_link_libraries(texture_animation_tests PRIVATE charged_static_inventory)
    add_test(NAME texture_animation COMMAND texture_animation_tests)
    set_tests_properties(texture_animation PROPERTIES TIMEOUT 30)
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
include(cmake/WorldSceneResources.cmake)
include(cmake/FrontendWorldFiles.cmake)
include(cmake/FrontendInput.cmake)
include(cmake/FrontendVisuals.cmake)
include(cmake/FrontendScene.cmake)
include(cmake/FrontendTextCatalog.cmake)
include(cmake/FrontendImages.cmake)
include(cmake/FrontendLayout.cmake)
add_library(charged_frontend_layout_gx STATIC src/runtime/frontend_layout_gx.cpp src/runtime/frontend_image_gx.cpp)
target_link_libraries(charged_frontend_layout_gx PUBLIC charged_frontend_layout charged_frontend_text_gx PRIVATE aurora::gx)
include(cmake/FrontendAnimation.cmake)
include(cmake/FrontendSession.cmake)
include(cmake/Hierarchy.cmake)
include(cmake/EffectsBundle.cmake)
include(cmake/SAnimAssets.cmake)
include(cmake/AnimationBundle.cmake)
include(cmake/PoseAccumulator.cmake)
include(cmake/AnimationPose.cmake)
include(cmake/ModelParticles.cmake)
include(cmake/SkinPose.cmake)
include(cmake/SkinRender.cmake)
include(cmake/AudioBankLoad.cmake)
include(cmake/AudioDsp.cmake)
include(cmake/AudioOutput.cmake)
include(cmake/GraphicsStartup.cmake)
include(cmake/ParticleRendering.cmake)
include(cmake/WorldEffects.cmake)
include(cmake/FrontendFontRegistry.cmake)
include(cmake/FrontendPackets.cmake)
include(cmake/FrontendHandler.cmake)
include(cmake/FrontendStack.cmake)
include(cmake/FrontendTransition.cmake)
include(cmake/FrontendPointer.cmake)
include(cmake/FrontendPointerHost.cmake)
include(cmake/FrontendPointerDisplay.cmake)
include(cmake/FrontendBootLoading.cmake)
include(cmake/AudioVoices.cmake)
include(cmake/FrontendAudio.cmake)
include(cmake/FrontendMusic.cmake)
include(cmake/FrontendMainMenu.cmake)
include(cmake/FrontendOptions.cmake)
include(cmake/FrontendNavigation.cmake)
include(cmake/FrontendOptionsNavigation.cmake)
include(cmake/FrontendMenuTransition.cmake)
include(cmake/NisPip.cmake)
add_library(charged_nis_pip_scene STATIC src/runtime/nis_pip_scene.cpp)
target_link_libraries(charged_nis_pip_scene PUBLIC charged_nis_pip charged_nis_pip_render charged_static_inventory PRIVATE aurora::gx)
add_library(charged_scene_preview STATIC src/runtime/scene.cpp)
target_compile_features(charged_scene_preview PRIVATE cxx_std_20)
target_include_directories(charged_scene_preview PUBLIC src)
target_link_libraries(charged_scene_preview PRIVATE charged_nis_pip_scene charged_graphics_startup charged_frontend_world_files charged_animated_camera charged_debug_camera_input charged_frames charged_frame_timing charged_shadows charged_compressed_assets charged_static_inventory charged_decomp_startup
    charged_world_scene charged_world_objects charged_host aurora::gx aurora::mtx aurora::os aurora::vi aurora::dvd aurora::core mscharged_build_info)
target_link_libraries(mscharged PRIVATE charged_scene_preview)
target_link_libraries(charged_scene_preview PRIVATE charged_frontend_visuals charged_frontend_font_registry charged_frontend_text_catalog charged_frontend_layout_gx charged_frontend_input charged_frontend_session charged_particle_render)
target_link_libraries(charged_scene_preview PRIVATE charged_frontend_packets charged_frontend_handler charged_frontend_boot_loading charged_audio_bank_load charged_skin_render)
target_link_libraries(charged_scene_preview PRIVATE charged_frontend_pointer_display)
target_link_libraries(charged_scene_preview PRIVATE charged_frontend_main_menu charged_frontend_options)
target_link_libraries(charged_scene_preview PRIVATE charged_frontend_navigation charged_frontend_stack)
target_link_libraries(charged_scene_preview PRIVATE charged_frontend_options_navigation)
target_link_libraries(charged_scene_preview PRIVATE charged_frontend_music)
target_compile_definitions(mscharged PRIVATE MSCHARGED_HAS_SCENE_PREVIEW=1)
if(BUILD_TESTING)
    include(cmake/Tasks.cmake)
    add_executable(world_scene_tests tests/world_scene.cpp)
    target_link_libraries(world_scene_tests PRIVATE charged_world_scene)
    add_test(NAME world_scene COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_world_scene.py"
        --reader "$<TARGET_FILE:world_scene_tests>")
    set_tests_properties(world_scene PROPERTIES TIMEOUT 30)
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
    add_test(NAME effects_material_pipeline COMMAND material_pipeline_tests --effects-only)
    set_tests_properties(effects_material_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME frontend_boot_scene COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend_boot_scene.py" "$<TARGET_FILE:mscharged>" "$<TARGET_FILE:frontend_boot_audio_tests>")
    set_tests_properties(frontend_boot_scene PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation;SDL_AUDIODRIVER=dummy" RESOURCE_LOCK gx_check)
    add_test(NAME particle_preview_synthetic COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_particle_preview.py" "$<TARGET_FILE:mscharged>")
    set_tests_properties(particle_preview_synthetic PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
    add_executable(frontend_animation_pipeline_tests tests/frontend_animation_pipeline.cpp src/runtime/gpu_readback.cpp)
    target_link_libraries(frontend_animation_pipeline_tests PRIVATE charged_frontend_animation charged_frontend_layout_gx aurora::gx aurora::vi aurora::core)
    add_test(NAME frontend_animation_pipeline COMMAND frontend_animation_pipeline_tests)
    set_tests_properties(frontend_animation_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_executable(frontend_image_pipeline_tests tests/frontend_image_pipeline.cpp src/runtime/gpu_readback.cpp)
    target_link_libraries(frontend_image_pipeline_tests PRIVATE charged_frontend_layout_gx aurora::gx aurora::vi aurora::core)
    add_test(NAME frontend_image_pipeline COMMAND frontend_image_pipeline_tests)
    set_tests_properties(frontend_image_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME frontend_frame_synthetic COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend_frame.py" "$<TARGET_FILE:mscharged>")
    set_tests_properties(frontend_frame_synthetic PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
    add_executable(frontend_frame_pipeline_tests tests/frontend_frame_pipeline.cpp src/runtime/gpu_readback.cpp)
    target_link_libraries(frontend_frame_pipeline_tests PRIVATE charged_frontend_layout_gx aurora::gx aurora::vi aurora::core)
    add_test(NAME frontend_frame_pipeline COMMAND frontend_frame_pipeline_tests)
    set_tests_properties(frontend_frame_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME nis_pip_scene_synthetic COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nis_pip_scene.py" "$<TARGET_FILE:mscharged>")
    set_tests_properties(nis_pip_scene_synthetic PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
    add_executable(nis_pip_pipeline_tests tests/nis_pip_pipeline.cpp)
    target_link_libraries(nis_pip_pipeline_tests PRIVATE charged_nis_pip_scene aurora::gx aurora::vi aurora::core)
    add_test(NAME nis_pip_pipeline COMMAND nis_pip_pipeline_tests)
    set_tests_properties(nis_pip_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_executable(debug_camera_scene_tests tests/debug_camera_scene.cpp)
    target_link_libraries(debug_camera_scene_tests PRIVATE charged_scene_preview charged_frames charged_cameras SDL3::SDL3)
    add_test(NAME debug_camera_scene COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_debug_camera_scene.py"
        "$<TARGET_FILE:debug_camera_scene_tests>")
    set_tests_properties(debug_camera_scene PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
    add_test(NAME world_scene_synthetic COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_world_scene.py"
        --gpu "$<TARGET_FILE:mscharged>")
    set_tests_properties(world_scene_synthetic PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
    add_test(NAME frontend_layout_synthetic COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frontend_layout.py"
        "$<TARGET_FILE:mscharged>")
    set_tests_properties(frontend_layout_synthetic PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
    add_test(NAME texture_animation_pipeline COMMAND material_pipeline_tests --texture-animation-only)
    set_tests_properties(texture_animation_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME scrolling_camera_overlay_pipeline COMMAND material_pipeline_tests --scrolling-camera-only)
    set_tests_properties(scrolling_camera_overlay_pipeline PROPERTIES TIMEOUT 150 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME scrolling_masked_detail_pipeline COMMAND material_pipeline_tests --scrolling-masked-detail-only)
    set_tests_properties(scrolling_masked_detail_pipeline PROPERTIES TIMEOUT 150 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME masked_detail_pipeline COMMAND material_pipeline_tests --masked-detail-only)
    set_tests_properties(masked_detail_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
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
