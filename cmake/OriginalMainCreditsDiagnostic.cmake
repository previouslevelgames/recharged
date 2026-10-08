include_guard(GLOBAL)

# Owner-authorized original-main/selected-Credits diagnostic. Normal game
# inventories do not enable these temporary flow gates. One source module owns
# original main, FE/font/resources, handlers and rendering; the host supplies one
# actual SDK and window/device services before original construction.
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    message(FATAL_ERROR "The original-main source diagnostic requires Linux or macOS LP64 with GCC or Clang")
endif()
include(cmake/OriginalModuleLinkage.cmake)
include(cmake/OriginalFunctionPools.cmake)
include(cmake/WiiStringFormat.cmake)
include(cmake/NativeSystemSettings.cmake)
include(cmake/NativeSTM.cmake)
include(cmake/NativeVideo.cmake)
include(cmake/OriginalCreditsMovieHardware.cmake)
include(cmake/NativeHardwareOwner.cmake)
include(cmake/NativeVideoOutput.cmake)
include(cmake/NativeFilesystemBoot.cmake)
find_package(Threads REQUIRED)

add_library(mscharged_original_main_credits_module MODULE
    "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFunctionMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlString.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlPrint.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlDebug.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFileGC.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFile.cpp"
    src/platform/game_module_allocations.cpp
    src/platform/file_handle_abi.cpp
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLInventory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glResourcePool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/gl.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gc/gcSwizzler.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLVertexAnim.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Debug/FrameCounter.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlRandom.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
    src/platform/game_allocation_ownership.cpp
    src/platform/original_entry.cpp
    "${MSCHARGED_PREPARED}/src/NL/nlInit.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTicker.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTime.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Core/mtRandom.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemoryInit.cpp"
    src/runtime/sanim_decode.cpp
    "${MSCHARGED_PREPARED}/src/Game/main.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxTexture.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakRegistry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValueBase.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNode.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakEntry.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakNameRecycler.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakValue.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakCallback.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GameTweaks.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AIPad.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Game.cpp"
    src/platform/tweak_storage.cpp
    "${MSCHARGED_PREPARED}/src/Game/Ball.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glPlat.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glStat.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glDrawSyncLog.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glState.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTarget.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glView.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxGX.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSwap.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxTarget.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMaterialProgramRegistry.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXUnlitTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glStruct.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXUnlitTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXVertexColourTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXFloatTexturedColourMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXScissoredVertexColourTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialParameters.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glFont.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxFont.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glRenderList.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platvmath.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxModel.cpp"
    src/platform/game_resource_records.cpp
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
    src/platform/frontend_package.cpp
    "${MSCHARGED_PREPARED}/src/Game/Render/Frustum.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/RLViewLayers.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxFog.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/RLView.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glLoadModel.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxDisplayList.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSkinMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/glModelBuilder.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTexturedColourMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLFloatTexturedColourMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glModel.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxSend.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMath.cpp"
    "${MSCHARGED_PREPARED}/src/NL/math.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plane.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/platqmath.cpp"
    src/platform/rlg_record_abi.cpp
    src/platform/rlg_vertex_animation_abi.cpp
    src/platform/rlg_geometry_bytes.cpp
    src/platform/rlg_material_parameters.cpp
    src/platform/world_record_wire.cpp
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
    "${MSCHARGED_PREPARED}/src/Game/FE/feSceneResource.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feTextureResource.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTask.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlLocalization.cpp"
    src/platform/localization_data.cpp
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw2.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glDraw3.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMatrix.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTextBox.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Font/FontLoading.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GameInfo.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlConfig.cpp"
 )
set_source_files_properties("${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c" PROPERTIES LANGUAGE CXX)
add_dependencies(mscharged_original_main_credits_module verify_prepared)
include(cmake/OriginalCompressedFiles.cmake)
include(cmake/OriginalARC.cmake)
include(cmake/OriginalRFLShape.cmake)
include(cmake/OriginalRFLResource.cmake)
include(cmake/OriginalRFLCharacterSources.cmake)
include(cmake/OriginalCharacterLoading.cmake)
include(cmake/OriginalAnimationControllers.cmake)
mscharged_add_original_inflater(mscharged_original_main_credits_module)
set_target_properties(mscharged_original_main_credits_module PROPERTIES
    PREFIX "" POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(mscharged_original_main_credits_module PRIVATE cxx_std_20)
target_link_libraries(mscharged_original_main_credits_module PRIVATE charged_original_function_pool_abi)
target_include_directories(mscharged_original_main_credits_module BEFORE PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_include_directories(mscharged_original_main_credits_module PRIVATE
    "${MSCHARGED_PREPARED}/src" "${MSCHARGED_PREPARED}/src/NL/gl"
    "${MSCHARGED_PREPARED}/src/NL/glx")
target_compile_definitions(mscharged_original_main_credits_module PRIVATE
    MSCHARGED_GAME_MODULE=1 AURORA_WII_CLOCK=1 dSINGLE=1 __alloca=__builtin_alloca
    MSCHARGED_DIAGNOSTIC_MAIN_BOOTSTRAP=1 MSCHARGED_DIAGNOSTIC_MAIN_FRONTEND=1
    MSCHARGED_DIAGNOSTIC_MAIN_FRONTEND_SCENE=1 MSCHARGED_DIAGNOSTIC_CREDITS_SCENE=1
    MSCHARGED_DIAGNOSTIC_CREDITS_MOVIE=1 MSCHARGED_DIAGNOSTIC_CREDITS_COPYRIGHTS=1
    MSCHARGED_DIAGNOSTIC_MAIN_INPUT=1
    C_MTXFrustum=Charged_C_MTXFrustum C_MTXOrtho=Charged_C_MTXOrtho)
target_compile_options(mscharged_original_main_credits_module PRIVATE
    -O1 -ffunction-sections -fdata-sections -fno-strict-aliasing
    -ffp-contract=off -fsigned-char -Wno-unknown-pragmas -Wno-invalid-offsetof
    -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(mscharged_original_main_credits_module PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete
        -fno-devirtualize-speculatively)
else()
    target_compile_options(mscharged_original_main_credits_module PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
if(APPLE)
    # Original source remains incomplete: defer only unexecuted function imports.
    # Explicit exports keep game operators and the original codec module-local.
    target_link_options(mscharged_original_main_credits_module PRIVATE
        LINKER:-undefined,dynamic_lookup LINKER:-dead_strip LINKER:-no_fixup_chains)
else()
    target_link_options(mscharged_original_main_credits_module PRIVATE
        -Wl,-Bsymbolic-functions -Wl,--gc-sections)
endif()
mscharged_set_original_module_exports(mscharged_original_main_credits_module
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_main_credits_exports.map")

# Actual source InitPads/FEInput at their original Initialize positions. Keep
# source branches and whole TUs; ordinary O2 inlining matches the earlier bounded
# source qualification and does not retain an unused unfinished base-vptr store.
add_library(mscharged_original_main_credits_focus OBJECT
    "${MSCHARGED_PREPARED}/src/Game/PadActions.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/globalpad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/cGlobalPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/PadBackend.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feInput.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/GameCubePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiRemotePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiFreestylePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/cPlatPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/PlatPadManager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiClassicPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/SwappablePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/DPDData.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/kpad/KPAD.c"
    "${MSCHARGED_PREPARED}/src/Game/PadMonkey.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiPadMonkey.cpp"
    "${MSCHARGED_PREPARED}/src/Game/TweakConfig.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feHelpFuncs.cpp"
 )
add_dependencies(mscharged_original_main_credits_focus verify_prepared)
set_target_properties(mscharged_original_main_credits_focus PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(mscharged_original_main_credits_focus PRIVATE cxx_std_20)
target_link_libraries(mscharged_original_main_credits_focus PRIVATE charged_original_function_pool_abi)
target_include_directories(mscharged_original_main_credits_focus BEFORE PRIVATE "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(mscharged_original_main_credits_focus PRIVATE
    MSCHARGED_GAME_MODULE=1 AURORA_WII_CLOCK=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(mscharged_original_main_credits_focus PRIVATE
    -O2 -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off
    -fsigned-char -Wno-unknown-pragmas -Wno-invalid-offsetof -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(mscharged_original_main_credits_focus PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete -fno-devirtualize-speculatively)
else()
    target_compile_options(mscharged_original_main_credits_focus PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
target_link_libraries(mscharged_original_main_credits_module PRIVATE mscharged_original_main_credits_focus)

# Target-local whole SDK VI owns the true native clock/IRQ implementation. The
# default aurora_vi archive stays ordinary (never whole-linked here), so its
# alternate VI object is not pulled after this complete provider resolves VI.
# Preserve the normal/standalone diagnostic build's existing feature selection.
add_library(mscharged_original_main_credits_vi OBJECT
    "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/vi/vi.cpp")
add_dependencies(mscharged_original_main_credits_vi verify_prepared)
target_compile_features(mscharged_original_main_credits_vi PRIVATE cxx_std_20)
target_compile_definitions(mscharged_original_main_credits_vi PRIVATE
    AURORA_NATIVE_VIDEO=1 AURORA_WII_CLOCK=1 TARGET_PC=1)
target_compile_options(mscharged_original_main_credits_vi PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(mscharged_original_main_credits_vi PRIVATE aurora::vi)

add_library(charged_original_main_credits_host OBJECT
    src/runtime/original_main_credits.cpp
    src/platform/os.cpp src/platform/host_metadata.cpp src/platform/string_format.cpp
    src/platform/report.cpp src/platform/thread.cpp src/platform/os_version.cpp)
add_dependencies(charged_original_main_credits_host mscharged_original_main_credits_module)
target_compile_features(charged_original_main_credits_host PRIVATE cxx_std_20)
target_include_directories(charged_original_main_credits_host BEFORE PRIVATE "${MSCHARGED_AURORA_PREPARED}/include")
target_include_directories(charged_original_main_credits_host PUBLIC src)
target_include_directories(charged_original_main_credits_host PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/lib"
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_original_main_credits_host PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1
    MSCHARGED_ORIGINAL_MAIN_CREDITS_MODULE_FILENAME="$<TARGET_FILE_NAME:mscharged_original_main_credits_module>")
target_compile_options(charged_original_main_credits_host PRIVATE
    -O2 -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off)
target_link_libraries(charged_original_main_credits_host PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,aurora::gx,aurora::mtx,aurora::os>"
    aurora::core aurora::dvd charged_host charged_wii_string_format charged_native_stm
    charged_native_system_settings charged_native_video_device charged_credits_movie_hardware
    charged_native_hardware_owner
    charged_native_video_output_device charged_native_filesystem_boot
    charged_native_ipc_boot_buffer Threads::Threads ${CMAKE_DL_LIBS})

function(mscharged_link_original_main_credits target)
    add_dependencies(${target} mscharged_original_main_credits_module)
    target_link_libraries(${target} PRIVATE charged_original_main_credits_host
        mscharged_original_main_credits_vi aurora::core)
    if(APPLE)
        target_link_options(${target} PRIVATE LINKER:-dead_strip LINKER:-export_dynamic
            LINKER:-unexported_symbol,___OSHotReset
            LINKER:-unexported_symbol,___OSShutdownToSBY
            LINKER:-unexported_symbol,___OSSetVIForceDimming)
    else()
        target_link_options(${target} PRIVATE
            -Wl,--gc-sections -Wl,--export-dynamic
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_main_credits_host_exports.map")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_main_credits_host_exports.map")
    endif()
    mscharged_require_original_host_symbol(${target} PRIVATE SCGetSimpleAddressID)
    set_property(TARGET ${target} PROPERTY LINK_LIBRARY_OVERRIDE
        "WHOLE_ARCHIVE,aurora_gx,aurora_mtx,aurora_os")
endfunction()

add_executable(mscharged-original-main-credits-check
    tests/diagnostics/original_main_credits_window.cpp)
mscharged_link_original_main_credits(mscharged-original-main-credits-check)

# The user opts into this combined executable by enabling both existing options.
# The graphics preset's ordinary launcher-OFF preference is never changed here.
if(TARGET mscharged)
    target_compile_definitions(mscharged PRIVATE MSCHARGED_HAS_ORIGINAL_CREDITS=1)
    mscharged_link_original_main_credits(mscharged)
endif()

if(BUILD_TESTING AND MSCHARGED_TEST_VULKAN AND MSCHARGED_CREDITS_TEST_DISC)
    add_test(NAME original_main_credits_vulkan COMMAND mscharged-original-main-credits-check
        --disc "${MSCHARGED_CREDITS_TEST_DISC}")
    set_tests_properties(original_main_credits_vulkan PROPERTIES TIMEOUT 70
        LABELS "gpu;vulkan;owned-data" RESOURCE_LOCK gx_check
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|Actual source diagnostic stopped:")
endif()

include(cmake/OriginalLoadingDiagnostic.cmake)

if(MSCHARGED_BUILD_ORIGINAL_FRONTEND_DIAGNOSTIC)
    include(cmake/OriginalFrontendDiagnostic.cmake)
endif()
