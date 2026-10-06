include_guard(GLOBAL)

# Linux LP64/ELF source-module compiler graph. This is not a game library link
# or entry/runtime gate. Full source/static/provider closure remains in progress;
# inherited unreviewed source patches do not become approved by compilation.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()

include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_game_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFunctionMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlEvent.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlBind.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlString.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlPrint.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlInit.cpp"
    "${MSCHARGED_PREPARED}/src/Game/main.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Game.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/DispatchEventsTask.cpp"
    src/platform/game_allocation_ownership.cpp
    src/platform/game_module_allocations.cpp)
add_dependencies(charged_original_game_module_sources verify_prepared)
set_target_properties(charged_original_game_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_game_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_game_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
# Only real source ABI and host foundations. Legacy core/function_memory,
# configuration, tweak, task and scene replicas are not module providers.
# These objects must be isolated from the host executable/SDK operators. The
# future module imports one host foundation; this inventory does not link it.
target_link_libraries(charged_original_game_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_game_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_game_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new)
# Original CurrentAllocator scopes and null-returning class pools are visible
# to the compiler; do not alter source allocation requests to retain them.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_game_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(charged_original_game_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
add_custom_target(charged_original_module_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_game_module_sources>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-module-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_game_module_sources
    COMMENT "Compile original module entry/core; full link, lifetime and startup pending"
    VERBATIM)

include(cmake/OriginalCameras.cmake)
include(cmake/OriginalConfig.cmake)

include(cmake/OriginalCredits.cmake)

include(cmake/OriginalInput.cmake)

include(cmake/OriginalFiles.cmake)

include(cmake/OriginalFontResources.cmake)

include(cmake/OriginalSkeleton.cmake)

include(cmake/OriginalStaticTasks.cmake)

include(cmake/OriginalEmissionModule.cmake)

include(cmake/OriginalImpostorModule.cmake)

include(cmake/OriginalFrontendModule.cmake)

include(cmake/OriginalPlatformGraphics.cmake)
