include_guard(GLOBAL)

# Whole original offline-initializer compiler inventory. This does not link or
# execute NetworkUpdateTask::Initialize; original source/global closure remains
# incomplete, and online wire/manager execution is unqualified. The message storage
# compiler cohort retains all original pool and serializer providers.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_network_producer_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Task/NetworkUpdateTask.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/SocketNetwork.cpp"
    "${MSCHARGED_PREPARED}/src/Game/InputRouter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/NetworkInput.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/TransportMessage.cpp"
    "${MSCHARGED_PREPARED}/src/Game/NetworkMessageSerializer.cpp")
add_dependencies(charged_original_network_producer_sources verify_prepared)
set_target_properties(charged_original_network_producer_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_network_producer_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_network_producer_sources PRIVATE
    "${MSCHARGED_PREPARED}" "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_network_producer_sources PRIVATE
    charged_original_function_pool_abi)
target_compile_definitions(charged_original_network_producer_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_network_producer_sources PRIVATE
    -ffunction-sections -fdata-sections -ffp-contract=off
    -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-rtti)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_network_producer_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_network_producer_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
