include_guard(GLOBAL)

# The complete original save TU supplies source decisions and requests. This
# inventory does not establish NAND, banner/TPL transport or working saves.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_save_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/DB/SaveLoad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlMain.cpp")
add_dependencies(charged_original_save_sources verify_prepared)
set_target_properties(charged_original_save_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_save_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_save_sources PRIVATE
    "${MSCHARGED_PREPARED}" "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_save_sources PRIVATE
    charged_original_function_pool_abi)
target_compile_definitions(charged_original_save_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_save_sources PRIVATE
    -ffunction-sections -fdata-sections -ffp-contract=off
    -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-rtti)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_save_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_save_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()

# SaveLoad retains Wii16 text and fixed32 TPL records beneath its source flow.
include(cmake/OriginalSaveIcons.cmake)
mscharged_add_original_save_icon_transport(charged_original_save_sources)

# The original checksum implementation and fixed saved UserInfo footprint.
# This byte/layout qualifier does not construct save owners or supply NAND.
if(BUILD_TESTING)
    add_executable(original_save_scalar_tests
        tests/original_save_scalar.cpp
        "${MSCHARGED_PREPARED}/src/NL/nlMain.cpp")
    add_dependencies(original_save_scalar_tests verify_prepared)
    target_compile_features(original_save_scalar_tests PRIVATE cxx_std_20)
    target_link_libraries(original_save_scalar_tests PRIVATE
        charged_original_function_pool_abi)
    target_include_directories(original_save_scalar_tests PRIVATE
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(original_save_scalar_tests PRIVATE
        MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
    target_compile_options(original_save_scalar_tests PRIVATE
        -ffunction-sections -fdata-sections -fno-strict-aliasing
        -ffp-contract=off -fsigned-char -Wno-unknown-pragmas -fcheck-new)
    target_link_options(original_save_scalar_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME original_save_scalars COMMAND original_save_scalar_tests)
    set_tests_properties(original_save_scalars PROPERTIES TIMEOUT 30)
endif()
