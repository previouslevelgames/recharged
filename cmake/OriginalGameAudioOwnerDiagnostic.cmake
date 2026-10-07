include_guard(GLOBAL)

# Constructor-only source admission. The original main owns its GameAudio at
# the original placement-new position; Backend.Initialize/bundle/AX/playback
# remain outside this option and require their own original predecessors.
option(MSCHARGED_DIAGNOSTIC_GAME_AUDIO_OWNER
    "Admit the original GameAudio constructor in the selected frontend diagnostic" OFF)
function(mscharged_select_original_game_audio_owner target)
    if(NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_OWNER)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE)
        message(FATAL_ERROR "GameAudio owner admission requires the named original frontend sequence")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original GameAudio providers belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_GAME_AUDIO_OWNER=1)
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN ITEMS
        src/NL/nlRegistry.cpp
        src/NL/nlRegistryLookup.cpp
        src/NL/nlRegistryOwner.cpp
        src/Game/Audio/AudioSystem.cpp
        src/Game/Audio/audio.cpp
        src/Game/Audio/AudioBundleManager.cpp
        src/Game/Audio/AudioBundleManagerPlatform.cpp
        src/Game/Audio/AudioBackend.cpp
        src/Game/Audio/Plat3dSoundSrc.cpp
        src/Game/Audio/AudioSource.cpp
        src/Game/Audio/AudioResourceRuntime.cpp
        src/Game/Audio/AudioResourcePlatform.cpp
        src/Game/Audio/AudioScriptRuntime.cpp
        src/Game/Audio/AudioEffectBinding.cpp
        src/Game/Audio/AudioSlider.cpp
        src/Game/Audio/AudioCalculation.cpp
        src/Game/Audio/AudioBankTable.cpp
        src/Game/Audio/AudioRpc.cpp
        src/Game/Audio/AudioRuntimeGroup.cpp
        src/Game/Audio/AudioEffects.cpp
        src/Game/Audio/LowPassFilter.cpp
        src/Game/Audio/Pitch.cpp
        src/Game/Audio/CategoryVolume.cpp
        src/Game/Audio/Delay.cpp
        src/Game/Audio/Reverb.cpp
        src/Game/Audio/AuxEffectMap.cpp
        src/Game/Audio/XSoundHandle.cpp
        src/Game/Audio/XSoundCueHandle.cpp
        src/Game/Audio/SoundInstance.cpp
        src/Game/Audio/AudioSequenceInstance.cpp
        src/Game/Audio/AudioSequenceEvent.cpp
        src/Game/Audio/AudioResourceLoadOwner.cpp
        src/Game/Audio/AudioResourceLoader.cpp
        src/Game/Audio/Transition.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
        endif()
    endforeach()
    foreach(_native_source IN ITEMS
        src/platform/native_packed_registry.cpp
        src/platform/native_audio_script.cpp
        src/platform/native_audio_controls.cpp
        src/platform/native_audio_bank.cpp
        src/platform/native_audio_resource.cpp
        src/platform/native_audio_rpc.cpp
        src/platform/native_audio_memory.cpp
        src/platform/native_audio_stream.cpp)
        if(NOT _native_source IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_native_source}")
        endif()
    endforeach()
    # Existing source pool/allocator, interpreter, event, focus and SDK owner
    # services retain their real providers. No SDK/AX initialization is added.
endfunction()
