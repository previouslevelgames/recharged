include_guard(GLOBAL)

# Temporary, explicitly selected original-scene integration tool. The module
# executes retail FE/font/resource/drawing and THP movie source. Main/tasks,
# AX audio predecessor, physical input/world and source VI scanout remain held.
# The named movie diagnostic selects real THPSimple mode0; normal mode1 remains.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    message(FATAL_ERROR "The original Credits diagnostic currently requires Linux LP64 with GCC or Clang")
endif()
if(NOT TARGET aurora::dvd OR NOT TARGET aurora::gx)
    message(FATAL_ERROR "The original Credits diagnostic requires the real GX and DVD providers")
endif()

include(cmake/OriginalFunctionPools.cmake)
include(cmake/WiiStringFormat.cmake)
include(cmake/NativeSystemSettings.cmake)
include(cmake/NativeVideo.cmake)
include(cmake/OriginalCreditsMovieHardware.cmake)
find_package(Threads REQUIRED)

# Compile the exact source path in an isolated game module. Do not link native
# replacement game managers or a second SDK/allocator instance into this module.
add_library(mscharged_original_credits_module MODULE
    "${MSCHARGED_PREPARED}/src/Game/FE/feSceneManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feRender.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePackage.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePresentation.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feLibObject.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feText.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feImage.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feAnimation.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feFinder.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feGroup.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feLayer.cpp"
    "${MSCHARGED_PREPARED}/src/NL/utility.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlSlide.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlComponent.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlComponentInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlTextInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlTextInstance_runtime.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlDefault.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/BaseGameSceneManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHOptions.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Sys/simpleparser.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlEvent.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlBind.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHCredits.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHMoviePlayer.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Sys/movie.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/thp/THPSimple.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/GameRenderTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLMovieMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMovieMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMovieMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSwap.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlConfig.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/Frustum.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxTexture.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/RLViewLayers.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxFog.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxTarget.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/RLView.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTarget.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFunctionMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlString.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlPrint.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLInventory.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLVertexAnim.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glResourcePool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glLoadModel.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/gl.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxModel.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxDisplayList.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSkinMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gc/gcSwizzler.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlDebug.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakRegistry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValueBase.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNode.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakEntry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNameRecycler.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValue.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakCallback.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GameTweaks.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Game.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AIPad.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/glModelBuilder.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTexturedColourMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLFloatTexturedColourMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glState.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glStruct.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glPlat.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxGX.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialParameters.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXUnlitTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXUnlitTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glModel.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemoryInit.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSend.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/math.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plane.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platvmath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platqmath.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c"
    "${MSCHARGED_PREPARED}/src/NL/nlFileGC.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFile.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Font/fontmanager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFont.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTextEscape.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlBundleFile.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feResourceManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feFontResource.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feScene.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/BaseSceneHandler.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/FrontEndTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Team.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AI/StatsGatherer.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feSceneResource.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feTextureResource.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTask.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlLocalization.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glView.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glRenderList.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw2.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw3.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTextBox.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMaterialProgramRegistry.cpp"
    src/platform/frontend_package.cpp
    src/platform/game_resource_records.cpp
    src/platform/game_allocation_ownership.cpp
    src/platform/game_module_allocations.cpp
    src/platform/rlg_record_abi.cpp
    src/platform/rlg_vertex_animation_abi.cpp
    src/platform/rlg_geometry_bytes.cpp
    src/platform/rlg_material_parameters.cpp
    src/platform/tweak_storage.cpp
    src/platform/file_handle_abi.cpp
    src/platform/localization_data.cpp
    tests/diagnostics/credits_font.cpp
    tests/diagnostics/credits_scene.cpp
    tests/diagnostics/credits_trace.cpp
)
set_source_files_properties("${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c" PROPERTIES LANGUAGE CXX)
add_dependencies(mscharged_original_credits_module verify_prepared)
include(cmake/OriginalCompressedFiles.cmake)
mscharged_add_original_inflater(mscharged_original_credits_module)
set_target_properties(mscharged_original_credits_module PROPERTIES
    PREFIX "" POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(mscharged_original_credits_module PRIVATE cxx_std_20)
target_link_libraries(mscharged_original_credits_module PRIVATE charged_original_function_pool_abi)
target_include_directories(mscharged_original_credits_module PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include"
    "${MSCHARGED_PREPARED}/src"
    "${MSCHARGED_PREPARED}/src/NL/gl"
    "${MSCHARGED_PREPARED}/src/NL/glx")
target_compile_definitions(mscharged_original_credits_module PRIVATE
    MSCHARGED_DIAGNOSTIC_CREDITS_SCENE=1 MSCHARGED_DIAGNOSTIC_CREDITS_MOVIE=1 MSCHARGED_DIAGNOSTIC_CREDITS_COPYRIGHTS=1
    MSCHARGED_GAME_MODULE=1
    AURORA_WII_CLOCK=1 dSINGLE=1 __alloca=__builtin_alloca
    C_MTXFrustum=Charged_C_MTXFrustum C_MTXOrtho=Charged_C_MTXOrtho)
# O2/no-inline is the qualified bounded diagnostic policy, independently of the
# normal compiler-only game inventories and their selected build optimization.
target_compile_options(mscharged_original_credits_module PRIVATE
    -O2 -fno-inline -ffunction-sections -fdata-sections -fno-strict-aliasing
    -ffp-contract=off -fsigned-char -Wno-unknown-pragmas -Wno-invalid-offsetof
    -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(mscharged_original_credits_module PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete
        # Preserve genuine virtual calls without retaining speculative branches
        # into unrelated IntroMovieScene/world providers in this bounded gate.
        -fno-devirtualize-speculatively)
else()
    target_compile_options(mscharged_original_credits_module PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
target_link_options(mscharged_original_credits_module PRIVATE
    -Wl,-Bsymbolic-functions -Wl,--gc-sections
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/credits_exports.map"
    -Wl,--wrap=_Z15glResourceAllocm9eGLMemoryPv
    -Wl,--wrap=_ZN10BundleFile18GetFileInfoByIndexEmP24BundleFileDirectoryEntry)
set_property(TARGET mscharged_original_credits_module APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/credits_exports.map")

# True original logical-pad/focus prerequisites for scene-manager Update. The
# source InitPads creates eight null-backend logical pads; no physical input
# readiness or CreatePadBackends is substituted. Ordinary O2 inlining removes
# unused base-vptr stores; -fno-inline would retain the unfinished base vtable.
add_library(mscharged_original_credits_focus OBJECT
    "${MSCHARGED_PREPARED}/src/Game/PadActions.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/globalpad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/cGlobalPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/PadBackend.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feInput.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/GameCubePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiRemotePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiFreestylePad.cpp")
add_dependencies(mscharged_original_credits_focus verify_prepared)
set_target_properties(mscharged_original_credits_focus PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(mscharged_original_credits_focus PRIVATE cxx_std_20)
target_link_libraries(mscharged_original_credits_focus PRIVATE charged_original_function_pool_abi)
target_include_directories(mscharged_original_credits_focus PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include" "${MSCHARGED_PREPARED}/src")
target_compile_definitions(mscharged_original_credits_focus PRIVATE
    MSCHARGED_GAME_MODULE=1 AURORA_WII_CLOCK=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(mscharged_original_credits_focus PRIVATE
    -O2 -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off
    -fsigned-char -Wno-unknown-pragmas -Wno-invalid-offsetof -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(mscharged_original_credits_focus PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete -fno-devirtualize-speculatively)
else()
    target_compile_options(mscharged_original_credits_focus PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
target_link_libraries(mscharged_original_credits_module PRIVATE mscharged_original_credits_focus)

# Target-local actual SDK VI provides original retrace callbacks/movie clocks.
# Its complete object resolves VI before the ordinary SDK archive; never whole
# link that archive here, which would add a second VI implementation.
add_library(mscharged_original_credits_vi OBJECT
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/vi/vi.cpp")
add_dependencies(mscharged_original_credits_vi verify_prepared)
target_compile_features(mscharged_original_credits_vi PRIVATE cxx_std_20)
target_compile_definitions(mscharged_original_credits_vi PRIVATE
    AURORA_NATIVE_VIDEO=1 AURORA_WII_CLOCK=1 TARGET_PC=1)
target_compile_options(mscharged_original_credits_vi PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(mscharged_original_credits_vi PRIVATE aurora::vi)

# One genuine SDK instance lives in the host. Export its actual hardware and
# metadata symbols to the hidden module, leaving host STL on the host allocator.
add_executable(mscharged-original-credits-check
    tests/diagnostics/credits_window.cpp
    src/platform/os.cpp
    src/platform/host_metadata.cpp
    src/platform/string_format.cpp
    src/platform/report.cpp
    src/platform/thread.cpp)
add_dependencies(mscharged-original-credits-check mscharged_original_credits_module)
target_compile_features(mscharged-original-credits-check PRIVATE cxx_std_20)
target_include_directories(mscharged-original-credits-check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(mscharged-original-credits-check PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1
    MSCHARGED_CREDITS_MODULE_FILENAME="$<TARGET_FILE_NAME:mscharged_original_credits_module>")
target_compile_options(mscharged-original-credits-check PRIVATE
    -O2 -fno-inline -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off)
target_link_options(mscharged-original-credits-check PRIVATE -Wl,--gc-sections -Wl,--export-dynamic)
set_property(TARGET mscharged-original-credits-check PROPERTY LINK_LIBRARY_OVERRIDE
    "WHOLE_ARCHIVE,aurora_gx,aurora_mtx,aurora_os")
target_link_libraries(mscharged-original-credits-check PRIVATE
    mscharged_original_credits_vi
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,aurora::gx,aurora::mtx,aurora::os>"
    aurora::core aurora::dvd charged_wii_string_format
    charged_native_system_settings charged_native_video_device charged_credits_movie_hardware
    Threads::Threads ${CMAKE_DL_LIBS})

# An owned image and desktop GPU are explicit inputs, never part of portable CI.
set(MSCHARGED_CREDITS_TEST_DISC "" CACHE FILEPATH "Owned ISO/RVZ for the opt-in original Credits Vulkan test")
if(BUILD_TESTING AND MSCHARGED_TEST_VULKAN AND MSCHARGED_CREDITS_TEST_DISC)
    add_test(NAME original_credits_vulkan COMMAND mscharged-original-credits-check
        --disc "${MSCHARGED_CREDITS_TEST_DISC}")
    set_tests_properties(original_credits_vulkan PROPERTIES
        TIMEOUT 70 LABELS "gpu;vulkan;owned-data" RESOURCE_LOCK gx_check
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|Original Credits gate:")
endif()

# Same initialized source state, entered through original main.
include(cmake/OriginalMainCreditsDiagnostic.cmake)
