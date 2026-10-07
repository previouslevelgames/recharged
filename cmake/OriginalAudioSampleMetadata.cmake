include_guard(GLOBAL)

# Whole Matching loader/SP providers and native serialized-data/address ABI.
# This source inventory does not initialize Backend, AX, or audio playback.
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_audio_sample_metadata_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioResourcePlatform.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/sp/sp.c"
    src/platform/native_sp_table.cpp)
add_dependencies(charged_original_audio_sample_metadata_sources verify_prepared)
set_target_properties(charged_original_audio_sample_metadata_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_audio_sample_metadata_sources PRIVATE
    c_std_17 cxx_std_20)
target_include_directories(charged_original_audio_sample_metadata_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_audio_sample_metadata_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_audio_sample_metadata_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_audio_sample_metadata_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    "$<$<COMPILE_LANGUAGE:CXX>:-fcheck-new;-fno-rtti>")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_audio_sample_metadata_sources PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique;-fno-assume-sane-operators-new-delete>")
else()
    target_compile_options(charged_original_audio_sample_metadata_sources PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-assume-sane-operator-new;-Wno-register>")
endif()
