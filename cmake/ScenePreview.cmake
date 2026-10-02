include(cmake/NativeRuntime.cmake)
add_library(charged_materials STATIC
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialAlphaState.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GameObjectLightingCore.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/LightingLookupNative.cpp"
    src/runtime/materials.cpp src/runtime/material_gx.cpp src/runtime/material_texture.cpp
    src/runtime/gpu_readback.cpp src/runtime/lighting.cpp)
foreach(program UnlitTexture VertexColourTexture ScrollingDiffuse MaskedSpecularFresnel)
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
add_library(charged_static_inventory STATIC src/runtime/static_inventory.cpp)
target_compile_features(charged_static_inventory PUBLIC cxx_std_20)
target_include_directories(charged_static_inventory PUBLIC src)
target_link_libraries(charged_static_inventory PUBLIC charged_materials)
if(BUILD_TESTING)
    add_executable(static_inventory_tests tests/static_inventory.cpp)
    target_link_libraries(static_inventory_tests PRIVATE charged_static_inventory)
    add_test(NAME static_inventory COMMAND static_inventory_tests)
    set_tests_properties(static_inventory PROPERTIES TIMEOUT 30)
endif()
add_library(charged_scene_preview STATIC src/runtime/scene.cpp)
target_compile_features(charged_scene_preview PRIVATE cxx_std_20)
target_include_directories(charged_scene_preview PUBLIC src)
target_link_libraries(charged_scene_preview PRIVATE charged_static_inventory charged_decomp_startup
    charged_host aurora::gx aurora::mtx aurora::os aurora::vi aurora::dvd aurora::core mscharged_build_info)
target_link_libraries(mscharged PRIVATE charged_scene_preview)
target_compile_definitions(mscharged PRIVATE MSCHARGED_HAS_SCENE_PREVIEW=1)
if(BUILD_TESTING)
    add_executable(material_pipeline_tests tests/material_pipeline.cpp)
    target_link_libraries(material_pipeline_tests PRIVATE charged_static_inventory aurora::gx aurora::vi aurora::core)
endif()
if(BUILD_TESTING AND MSCHARGED_TEST_VULKAN)
    add_test(NAME material_pipeline COMMAND material_pipeline_tests)
    set_tests_properties(material_pipeline PROPERTIES TIMEOUT 120 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|FAILED:")
    add_test(NAME scene_synthetic
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scene.py" "$<TARGET_FILE:mscharged>")
    set_tests_properties(scene_synthetic PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation" RESOURCE_LOCK gx_check)
endif()
