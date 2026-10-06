include_guard(GLOBAL)

# Whole frontend/frame-resource source under the original game-module ABI.
# The compiler inventory does not establish full program startup, resources, scenes or CRT lifetime.
# Legacy matrix/state checks belong only to diagnostic consumers; this
# source module uses the original shared RTTI-off compiler profile.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()

add_library(charged_original_frontend_module OBJECT EXCLUDE_FROM_ALL
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
    "${MSCHARGED_PREPARED}/src/NL/gl/glTarget.cpp"
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
    "${MSCHARGED_PREPARED}/src/NL/gl/glFont.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxFont.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/mtx/mtx44.c"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLInventory.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/gl/glResourcePool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/glxTexture.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXUnlitTextureMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXUnlitTextureMaterialProgramRender.cpp"
    src/platform/game_resource_records.cpp)
add_dependencies(charged_original_frontend_module verify_prepared)
set_target_properties(charged_original_frontend_module PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_frontend_module PRIVATE cxx_std_20 c_std_99)
target_include_directories(charged_original_frontend_module PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_frontend_module PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_frontend_module PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca
    C_MTXFrustum=Charged_C_MTXFrustum C_MTXOrtho=Charged_C_MTXOrtho)
target_compile_options(charged_original_frontend_module PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    "$<$<COMPILE_LANGUAGE:CXX>:-fcheck-new>")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_frontend_module PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique;-fno-assume-sane-operators-new-delete>")
else()
    target_compile_options(charged_original_frontend_module PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-assume-sane-operator-new;-Wno-register>")
endif()
add_custom_target(charged_original_frontend_module_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_frontend_module>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-frontend-module-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_frontend_module
    COMMENT "Compile original frontend/frame resources; full startup and scenes pending"
    VERBATIM)

if(BUILD_TESTING)
    add_executable(original_target_layout_tests tests/original_target_layout.cpp)
    target_link_libraries(original_target_layout_tests PRIVATE charged_original_function_pool_abi)
    add_dependencies(original_target_layout_tests verify_prepared)
    add_test(NAME original_target_layout COMMAND original_target_layout_tests)
endif()

# Declared-source/SDK layout only; actual source pool lifetime proof is a
# separate private isolated-module diagnostic, not full game readiness.
if(BUILD_TESTING)
    add_executable(original_resource_header_abi_tests tests/original_resource_header_abi.cpp)
    target_include_directories(original_resource_header_abi_tests PRIVATE
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_link_libraries(original_resource_header_abi_tests PRIVATE charged_original_function_pool_abi)
    target_compile_definitions(original_resource_header_abi_tests PRIVATE TARGET_PC=1)
    add_dependencies(original_resource_header_abi_tests verify_prepared)
    add_test(NAME original_resource_header_abi COMMAND original_resource_header_abi_tests)
endif()
