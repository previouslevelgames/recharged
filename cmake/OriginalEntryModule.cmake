include_guard(GLOBAL)

# Whole original entry/data/fixed-task providers. This compiler inventory retains
# every task/update/data method; an actual original-main link/invocation remains
# dependent on the full source/SDK graph, with no replacement startup prefix.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_entry_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/AI/Variant.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AI/FuzzyVariant.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AI/Fielder.cpp"
    "${MSCHARGED_PREPARED}/src/Game/DetInput.cpp"
    "${MSCHARGED_PREPARED}/src/Game/HBMManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feSceneManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Transitions/ModelTransition.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GameInfo.cpp"
    "${MSCHARGED_PREPARED}/src/Game/DB/BasicGameInfo.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/FixedUpdateTask.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/original_entry.cpp")
add_dependencies(charged_original_entry_module_sources verify_prepared)
set_target_properties(charged_original_entry_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_entry_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_entry_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_entry_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os aurora::dvd)
target_compile_definitions(charged_original_entry_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_entry_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas -fcheck-new)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_entry_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_entry_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
