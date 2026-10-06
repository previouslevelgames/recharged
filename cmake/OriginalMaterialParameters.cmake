include_guard(GLOBAL)
# Whole original setter/skin caller and fixed-word/native-pointer layout proof.
# This compiler inventory executes no material, rendering or game readiness.
# The nine additional program records have qualified ABI metadata; their whole
# GX providers remain pending canonical SDK/array transport restoration.
add_library(charged_original_material_parameters OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/gl/glMaterialParameters.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/ShaderSkinMesh.cpp"
    tests/original_material_parameter_layout.cpp)
add_dependencies(charged_original_material_parameters verify_prepared)
target_compile_features(charged_original_material_parameters PRIVATE cxx_std_20)
target_include_directories(charged_original_material_parameters PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include" "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_material_parameters PRIVATE charged_original_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_material_parameters PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
    # The unchanged source uses the MWCC-era register storage specifier.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(charged_original_material_parameters PRIVATE -Wno-register)
    endif()
elseif(MSVC)
    target_compile_options(charged_original_material_parameters PRIVATE /Gy /Gw)
endif()
add_custom_target(charged_material_parameter_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_material_parameters>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-material-parameter-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_material_parameters
    COMMENT "Compile original material parameter ABI; rendering remains unqualified"
    VERBATIM)
