include_guard(GLOBAL)
add_library(charged_specular_material_reader STATIC src/resources/specular_material.cpp)
target_include_directories(charged_specular_material_reader PUBLIC src)
target_compile_features(charged_specular_material_reader PUBLIC cxx_std_20)
target_sources(charged_materials PRIVATE src/runtime/specular_material.cpp
    "${MSCHARGED_PREPARED}/src/NL/glx/GXSpecularMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXSpecularMaterialProgramRender.cpp")
target_link_libraries(charged_materials PUBLIC charged_specular_material_reader)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_specular_material_reader PRIVATE -ffp-contract=off -fno-strict-aliasing)
endif()
if(BUILD_TESTING)
    # The CPU source recorder uses the ELF linker's actual GX symbol wrapping.
    if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_executable(specular_material_tests tests/specular_material.cpp)
        target_link_libraries(specular_material_tests PRIVATE charged_materials aurora::gx aurora::mtx)
        foreach(symbol GXBegin GXCallDisplayList GXClearVtxDesc GXColor1x16 GXEnd
            GXInitLightAttn GXInitLightAttnA GXInitLightColor GXInitLightDistAttn
            GXInitLightPos GXInitSpecularDir GXInitTexObj GXInitTexObjCI GXInitTexObjLOD
            GXInitTlutObj GXLoadLightObjImm GXLoadNrmMtxImm GXLoadPosMtxImm GXLoadTexMtxImm
            GXLoadTexObj GXLoadTlut GXNormal1x16 GXParam1u8 GXPosition1x16 GXSetAlphaCompare
            GXSetAlphaUpdate GXSetArray GXSetBlendMode GXSetChanAmbColor GXSetChanCtrl
            GXSetChanMatColor GXSetColorUpdate GXSetCullMode GXSetCurrentMtx GXSetNumChans
            GXSetNumTevStages GXSetNumTexGens GXSetTevAlphaIn GXSetTevAlphaOp GXSetTevColor
            GXSetTevColorIn GXSetTevColorOp GXSetTevColorS10 GXSetTevDirect GXSetTevKAlphaSel
            GXSetTevKColor GXSetTevKColorSel GXSetTevOrder GXSetTevSwapMode GXSetTexCoordGen2
            GXSetTexCoordScaleManually GXSetVtxAttrFmt GXSetVtxDesc GXSetZCompLoc GXSetZMode
            GXTexCoord1x16)
            target_link_options(specular_material_tests PRIVATE "LINKER:--wrap=${symbol}")
        endforeach()
        target_link_options(specular_material_tests PRIVATE
            "LINKER:--wrap=_Z31RestoreGameObjectShadowLightingv")
        add_test(NAME specular_material COMMAND specular_material_tests)
        set_tests_properties(specular_material PROPERTIES TIMEOUT 30)
    endif()
    add_executable(specular_pipeline_tests tests/specular_pipeline.cpp)
    target_link_libraries(specular_pipeline_tests PRIVATE charged_views aurora::gx aurora::vi aurora::core)
    if(MSCHARGED_TEST_VULKAN)
        add_test(NAME specular_pipeline COMMAND specular_pipeline_tests)
        set_tests_properties(specular_pipeline PROPERTIES TIMEOUT 90 LABELS "gpu;vulkan"
            ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
            RESOURCE_LOCK gx_check FAIL_REGULAR_EXPRESSION "VUID-|Error:|Validation Error|FAILED:")
    endif()
endif()
