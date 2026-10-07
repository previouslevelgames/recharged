include_guard(GLOBAL)

# Whole original material compiler inventory. Compilation does not qualify
# original startup, shader execution, source resource backing or rendered output.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
include(cmake/OriginalMaterialSources.cmake)
mscharged_original_material_sources(_material_sources)
add_library(charged_original_material_program_sources OBJECT EXCLUDE_FROM_ALL
    ${_material_sources})
add_dependencies(charged_original_material_program_sources verify_prepared)
set_target_properties(charged_original_material_program_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_material_program_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_material_program_sources BEFORE PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_include_directories(charged_original_material_program_sources PRIVATE
    "${MSCHARGED_PREPARED}/src"
    "${MSCHARGED_PREPARED}/src/NL/gl"
    "${MSCHARGED_PREPARED}/src/NL/glx")
target_link_libraries(charged_original_material_program_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_material_program_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca
    C_MTXFrustum=Charged_C_MTXFrustum C_MTXOrtho=Charged_C_MTXOrtho)
target_compile_options(charged_original_material_program_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-rtti)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_material_program_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_material_program_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
add_custom_target(charged_original_material_program_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_material_program_sources>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-material-program-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_material_program_sources
    COMMENT "Compile 87 whole original material TUs; complete link/resource/GPU admission pending"
    VERBATIM)
