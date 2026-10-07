include_guard(GLOBAL)
# Entire reconstructed render providers. This object inventory supplies no
# replacement frame loop or platform startup and establishes no complete link.
add_library(charged_original_frontend_render OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/gl/gl.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/math.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plane.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platvmath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platqmath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glStruct.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glState.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxModel.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glView.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glRenderList.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw2.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw3.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialParameters.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/glModelBuilder.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTexturedColourMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLFloatTexturedColourMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxDisplayList.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSkinMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxGX.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSwap.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXConstantColourMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXConstantColourMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glFont.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c")
add_dependencies(charged_original_frontend_render verify_prepared)
target_compile_features(charged_original_frontend_render PRIVATE cxx_std_20)
set_target_properties(charged_original_frontend_render PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_include_directories(charged_original_frontend_render PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include" "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_frontend_render PRIVATE charged_original_core)
# Both SDKs provide projection entry points; select Charged's original formulas
# explicitly, keeping Aurora's separate 3x4 provider in the platform foundation.
target_compile_definitions(charged_original_frontend_render PRIVATE dSINGLE=1
    C_MTXFrustum=Charged_C_MTXFrustum C_MTXOrtho=Charged_C_MTXOrtho)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_frontend_render PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
elseif(MSVC)
    target_compile_options(charged_original_frontend_render PRIVATE /Gy /Gw)
endif()
add_custom_target(charged_frontend_render_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_frontend_render>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-frontend-render-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_frontend_render
    COMMENT "Record whole original render providers; not a scene/GPU readiness gate"
    VERBATIM)

# Qualified early-arena CPU fixture, with actual NL/font/FE providers and no
# allocation aliases. Clang Replay records and production module startup remain
# separate ABI gates, as with original_font_loading_tests.
if(BUILD_TESTING AND TARGET aurora::gx
    AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
    add_executable(original_frontend_render_tests
        tests/original_frontend_render.cpp tests/original_frontend_render_host.cpp
        "${MSCHARGED_PREPARED}/src/Game/Font/fontmanager.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlFont.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlTextEscape.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlBundleFile.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManager.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLInventory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glResourcePool.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxTexture.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/gl.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gc/gcSwizzler.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLVertexAnim.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlString.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlDebug.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakRegistry.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakValueBase.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakNode.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakEntry.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakNameRecycler.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakValue.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakCallback.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GameTweaks.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp"
        "${MSCHARGED_PREPARED}/src/Game/AIPad.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Game.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feResourceManager.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feFontResource.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feScene.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/BaseSceneHandler.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Task/FrontEndTask.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Team.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feSceneResource.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feTextureResource.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlTextBox.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlMath.cpp"
        "${MSCHARGED_PREPARED}/src/NL/math.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plane.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/platvmath.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/platqmath.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glStruct.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glState.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glMatrix.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxMatrix.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxModel.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glView.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glRenderList.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glDraw2.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glDraw3.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialProgram.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialParameters.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/glModelBuilder.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLTexturedColourMeshWriter.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLFloatTexturedColourMeshWriter.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxDisplayList.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxSkinMatrix.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxGX.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgram.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgramRender.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgram.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgramRender.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgram.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgramRender.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feRender.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/fePackage.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/fePresentation.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/tlSlide.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/tlComponent.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/tlComponentInstance.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/tlInstance.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/tlTextInstance.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/tlTextInstance_runtime.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feAnimation.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feLibObject.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feText.cpp"
        "${MSCHARGED_PREPARED}/src/NL/utility.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlLocalization.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c"
        src/platform/tweak_storage.cpp src/runtime/framebuffer_queries.cpp)
    target_compile_features(original_frontend_render_tests PRIVATE cxx_std_20)
    set_target_properties(original_frontend_render_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
    target_include_directories(original_frontend_render_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(original_frontend_render_tests PRIVATE dSINGLE=1
        __alloca=__builtin_alloca
        C_MTXFrustum=Charged_C_MTXFrustum C_MTXOrtho=Charged_C_MTXOrtho)
    # O1 is an explicit partial-link fixture policy: O3 Game.cpp generic
    # shared_ptr cleanup retains unqualified event-template providers. Original
    # libraries and the whole-TU object inventory retain profile optimizations.
    target_compile_options(original_frontend_render_tests PRIVATE
        -O1 -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
    target_link_options(original_frontend_render_tests PRIVATE -Wl,--gc-sections)
    target_link_libraries(original_frontend_render_tests PRIVATE
        charged_original_core charged_game_print aurora::dvd aurora::core
        aurora::gx aurora::mtx SDL3::SDL3)
    add_test(NAME original_frontend_render COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_frontend_render.py"
        "$<TARGET_FILE:original_frontend_render_tests>")
    set_tests_properties(original_frontend_render PROPERTIES TIMEOUT 120
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()

include(cmake/OriginalMaterialParameters.cmake)

include(cmake/OriginalLighting.cmake)

include(cmake/OriginalGXMaterials.cmake)

include(cmake/OriginalRLGRecords.cmake)
