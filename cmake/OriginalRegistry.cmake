include_guard(GLOBAL)

# Whole source registry/data compiler inventory. Runtime qualification uses an
# isolated original game module importing one host SDK; these objects alone do
# not establish AudioBundleManager, AX, original main or teardown readiness.
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_registry_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlRegistry.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlRegistryLookup.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlRegistryOwner.cpp"
    src/platform/native_packed_registry.cpp)
add_dependencies(charged_original_registry_module_sources verify_prepared)
set_target_properties(charged_original_registry_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_registry_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_registry_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_registry_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_registry_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_registry_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-rtti)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Literal RegistryIterator rebinding needs actual virtual dispatch here.
    # Scope the measured compiler fix to its defining whole source unit only.
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/NL/nlRegistryOwner.cpp"
        TARGET_DIRECTORY charged_original_registry_module_sources
        APPEND PROPERTY COMPILE_OPTIONS -fno-devirtualize)
    target_compile_options(charged_original_registry_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_registry_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
