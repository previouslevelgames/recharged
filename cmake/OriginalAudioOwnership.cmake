include_guard(GLOBAL)

# Genuine source owners and their live native ABI. These whole-source objects
# retain the real AudioSystem global/static requests; constructing its bundle
# still requires the original effect factory and actual Backend/AX services.
# The slider, script and resource-runtime TUs belong to their existing original
# inventories, with the same isolated original-game-module profile.
include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_audio_ownership_module_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/Audio/audio.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioBackend.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioSystem.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioBundleManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioBundleManagerPlatform.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioRpc.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/XSoundHandle.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/XSoundCueHandle.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/SoundInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioSequenceInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioSequenceEvent.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioResourceLoadOwner.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioResourceLoader.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Audio/AudioRuntimeGroup.cpp"
    src/platform/native_audio_memory.cpp
    src/platform/native_audio_rpc.cpp)
add_dependencies(charged_original_audio_ownership_module_sources verify_prepared)
set_target_properties(charged_original_audio_ownership_module_sources PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_audio_ownership_module_sources PRIVATE cxx_std_20)
target_include_directories(charged_original_audio_ownership_module_sources PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_audio_ownership_module_sources PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_audio_ownership_module_sources PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_audio_ownership_module_sources PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas -fcheck-new -fno-rtti)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_audio_ownership_module_sources PRIVATE
        -fno-gnu-unique -fno-assume-sane-operators-new-delete)
else()
    target_compile_options(charged_original_audio_ownership_module_sources PRIVATE
        -fno-assume-sane-operator-new -Wno-register)
endif()
