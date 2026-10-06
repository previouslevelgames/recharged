include_guard(GLOBAL)

# Whole original font/bundle/localization/FE resource TUs with the original
# allocation and function-pool ABI. This target owns no replacement managers.
# Its compiler inventory does not establish full main/scene startup; source
# font, queue and fallback execution is qualified separately in the module.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_font_resource_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Font/fontmanager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFont.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTextEscape.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlBundleFile.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlLocalization.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feResourceManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feFontResource.cpp"
    src/platform/localization_data.cpp)
add_dependencies(charged_original_font_resource_module_sources verify_prepared)
set_target_properties(charged_original_font_resource_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_font_resource_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_font_resource_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_font_resource_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os aurora::dvd)
target_compile_definitions(charged_original_font_resource_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_font_resource_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_font_resource_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_font_resource_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
