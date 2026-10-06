include_guard(GLOBAL)

# Complete reconstructed render providers only. The paired Program.cpp units
# still require real RLG array extent/endian and Warble FIFO transport; no
# unknown-size declarations or success forwarding are supplied by this scan.
add_library(charged_original_gx_material_render OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/glx/GXBlackTextureAlphaMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXCharacterDamageMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXColourFresnelMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMegaDiffuseMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMegaSpecularFresnelMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMegaSpecularMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXSkinnedMultiLightMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXSkinnedUnlitTextureMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXSpecularFresnelMaterialProgramRender.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXWarbleMaterialProgramRender.cpp")
add_dependencies(charged_original_gx_material_render verify_prepared)
target_compile_features(charged_original_gx_material_render PRIVATE cxx_std_20)
target_include_directories(charged_original_gx_material_render PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_gx_material_render PRIVATE charged_original_core)
target_compile_definitions(charged_original_gx_material_render PRIVATE
    dSINGLE=1 __alloca=__builtin_alloca)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_gx_material_render PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(charged_original_gx_material_render PRIVATE -Wno-register)
    endif()
elseif(MSVC)
    target_compile_options(charged_original_gx_material_render PRIVATE /Gy /Gw)
endif()
add_custom_target(charged_gx_material_render_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_gx_material_render>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-gx-material-render-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_gx_material_render
    COMMENT "Compile ten whole original GX render providers; arrays/loader/GPU remain pending"
    VERBATIM)
